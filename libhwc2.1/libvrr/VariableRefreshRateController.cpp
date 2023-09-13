/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)

#include "VariableRefreshRateController.h"

#include <android-base/logging.h>
#include <utils/Trace.h>

#include "ExynosHWCHelper.h"
#include "drmmode.h"

#include <chrono>
#include <tuple>

namespace android::hardware::graphics::composer {

namespace {

int64_t getNowNs() {
    const auto t = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

} // namespace

auto VariableRefreshRateController::CreateInstance(ExynosDisplay* display)
        -> std::shared_ptr<VariableRefreshRateController> {
    if (!display) {
        LOG(ERROR)
                << "VrrController: create VariableRefreshRateController without display handler.";
        return nullptr;
    }
    auto controller = std::shared_ptr<VariableRefreshRateController>(
            new VariableRefreshRateController(display));
    std::thread thread = std::thread(&VariableRefreshRateController::threadBody, controller.get());
    std::string threadName = "VrrCtrl_";
    threadName += display->mIndex == 0 ? "Primary" : "Second";
    int error = pthread_setname_np(thread.native_handle(), threadName.c_str());
    if (error != 0) {
        LOG(WARNING) << "VrrController: Unable to set thread name, error = " << strerror(error);
    }
    thread.detach();
    return controller;
}

VariableRefreshRateController::VariableRefreshRateController(ExynosDisplay* display)
      : mDisplay(display) {
    mState = VrrControllerState::kDisable;
    std::string displayFileNodePath = mDisplay->getPanelFileNodePath();
    if (displayFileNodePath.empty()) {
        LOG(WARNING) << "VrrController: Cannot find file node of display: "
                     << mDisplay->mDisplayName;
    } else {
        mFileNodeWritter = std::make_unique<FileNodeWriter>(displayFileNodePath);
    }
}

int VariableRefreshRateController::notifyExpectedPresent(int64_t timestamp,
                                                         int32_t frameIntervalNs) {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mRecord.mNextExpectedPresentTime = {mVrrActiveConfig, timestamp, frameIntervalNs};
        // Post kNotifyExpectedPresentConfig event.
        postEvent(VrrControllerEventType::kNotifyExpectedPresentConfig, getNowNs());
    }
    mCondition.notify_all();
    return 0;
}

void VariableRefreshRateController::reset() {
    ATRACE_CALL();

    const std::lock_guard<std::mutex> lock(mMutex);
    mEventQueue = std::priority_queue<VrrControllerEvent>();
    mRecord.clear();
    dropEventLocked();
}

void VariableRefreshRateController::setActiveVrrConfiguration(hwc2_config_t config) {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (mVrrConfigs.count(config) == 0) {
            LOG(ERROR) << "VrrController: Set an undefined active configuration";
            return;
        }
        mState = VrrControllerState::kRendering;
        mVrrActiveConfig = config;
        dropEventLocked(kRenderingTimeout);

        const auto& vrrConfig = mVrrConfigs[mVrrActiveConfig];
        postEvent(VrrControllerEventType::kRenderingTimeout,
                  getNowNs() + vrrConfig.notifyExpectedPresentConfig.TimeoutNs);
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::setEnable(bool isEnabled) {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (mEnabled == isEnabled) {
            return;
        }
        mEnabled = isEnabled;
        if (mEnabled == false) {
            dropEventLocked();
        }
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::setVrrConfigurations(
        std::unordered_map<hwc2_config_t, VrrConfig_t> configs) {
    ATRACE_CALL();

    const std::lock_guard<std::mutex> lock(mMutex);
    mVrrConfigs = std::move(configs);
}

void VariableRefreshRateController::stopThread() {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mThreadExit = true;
        mEnabled = false;
        mState = VrrControllerState::kDisable;
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::onPresent() {
    ATRACE_CALL();
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        if (!mRecord.mPendingCurrentPresentTime.has_value()) {
            LOG(WARNING) << "VrrController: VrrController: Present without expected present time "
                            "information";
            return;
        } else {
            LOG(INFO) << "VrrController: On present frame: time = "
                      << mRecord.mPendingCurrentPresentTime.value().time
                      << " duration = " << mRecord.mPendingCurrentPresentTime.value().duration;
            mRecord.mPresentHistory.next() = mRecord.mPendingCurrentPresentTime.value();
            mRecord.mPendingCurrentPresentTime = std::nullopt;
        }
        if (mState == VrrControllerState::kHibernate) {
            LOG(WARNING) << "VrrController: Present during hibernation without prior notification "
                            "via notifyExpectedPresent.";
            mState = VrrControllerState::kRendering;
            dropEventLocked(kHibernateTimeout);
        }
        // Drop the out of date timeout.
        dropEventLocked(kRenderingTimeout);
        dropEventLocked(kNextFrameInsertion);
        // Post next rendering timeout.
        postEvent(VrrControllerEventType::kRenderingTimeout,
                  getNowNs() + mVrrConfigs[mVrrActiveConfig].notifyExpectedPresentConfig.TimeoutNs);
    }
    mCondition.notify_all();
}

void VariableRefreshRateController::setExpectedPresentTime(int64_t timestampNanos,
                                                           int frameIntervalNs) {
    ATRACE_CALL();

    const std::lock_guard<std::mutex> lock(mMutex);
    mRecord.mPendingCurrentPresentTime = {mVrrActiveConfig, timestampNanos, frameIntervalNs};
}

void VariableRefreshRateController::onVsync(int64_t __unused timestampNanos,
                                            int32_t __unused vsyncPeriodNanos) {}

int VariableRefreshRateController::doFrameInsertionLocked() {
    ATRACE_CALL();
    static const std::string kNodeName = "refresh_ctrl";

    if (mPendingFramesToInsert <= 0) {
        LOG(ERROR) << "VrrController: the number of frames to be inserted should >= 1, but is "
                   << mPendingFramesToInsert << " now.";
        return -1;
    }
    bool ret = mFileNodeWritter->WriteCommandString(kNodeName, PANEL_REFRESH_CTRL_FI);
    if (!ret) {
        LOG(ERROR) << "VrrController: write command to file node failed.";
        return -1;
    }
    if (--mPendingFramesToInsert > 0) {
        postEvent(VrrControllerEventType::kNextFrameInsertion,
                  getNowNs() + mVrrConfigs[mVrrActiveConfig].minFrameIntervalNs);
    }
    return ret;
}

int VariableRefreshRateController::doFrameInsertionLocked(int frames) {
    mPendingFramesToInsert = frames;
    return doFrameInsertionLocked();
}

void VariableRefreshRateController::dropEventLocked() {
    mEventQueue = std::priority_queue<VrrControllerEvent>();
}

void VariableRefreshRateController::dropEventLocked(VrrControllerEventType event_type) {
    std::priority_queue<VrrControllerEvent> q;
    while (!mEventQueue.empty()) {
        const auto& it = mEventQueue.top();
        if (it.mEventType != event_type) {
            q.push(it);
        }
        mEventQueue.pop();
    }
    mEventQueue = std::move(q);
}

std::string VariableRefreshRateController::dumpEventQueueLocked() {
    std::string content;
    if (mEventQueue.empty()) {
        return content;
    }

    std::priority_queue<VrrControllerEvent> q;
    while (!mEventQueue.empty()) {
        const auto& it = mEventQueue.top();
        content += it.toString();
        content += "\n";
        q.push(it);
        mEventQueue.pop();
    }
    mEventQueue = std::move(q);
    return content;
}

int64_t VariableRefreshRateController::getNextEventTimeLocked() const {
    if (mEventQueue.empty()) {
        LOG(WARNING) << "VrrController: event queue should NOT be empty.";
        return -1;
    }
    const auto& event = mEventQueue.top();
    return event.mWhenNs;
}

std::string VariableRefreshRateController::getStateName(VrrControllerState state) const {
    switch (state) {
        case VrrControllerState::kDisable:
            return "Disable";
        case VrrControllerState::kRendering:
            return "Rendering";
        case VrrControllerState::kHibernate:
            return "Hibernate";
        default:
            return "Unknown";
    }
}

void VariableRefreshRateController::handleCadenceChange() {
    ATRACE_CALL();
    if (!mRecord.mNextExpectedPresentTime.has_value()) {
        LOG(WARNING) << "VrrController: cadence change occurs without the expected present timing "
                        "information.";
        return;
    }
    // TODO(cweichun): handle frame rate change.
    mRecord.mNextExpectedPresentTime = std::nullopt;
}

void VariableRefreshRateController::handleResume() {
    ATRACE_CALL();
    if (!mRecord.mNextExpectedPresentTime.has_value()) {
        LOG(WARNING)
                << "VrrController: resume occurs without the expected present timing information.";
        return;
    }
    // TODO(cweichun): handle panel resume.
    mRecord.mNextExpectedPresentTime = std::nullopt;
}

void VariableRefreshRateController::handleHibernate() {
    ATRACE_CALL();

    static constexpr int kNumFramesToInsert = 2;
    int ret = doFrameInsertionLocked(kNumFramesToInsert);
    LOG(INFO) << "VrrController: apply frame insertion, ret = " << ret;

    // TODO(cweichun): handle entering panel hibernate.
    postEvent(VrrControllerEventType::kHibernateTimeout,
              getNowNs() + kDefaultWakeUpTimeInPowerSaving);
}

void VariableRefreshRateController::handleStayHibernate() {
    ATRACE_CALL();
    // TODO(cweichun): handle keeping panel hibernate.
    postEvent(VrrControllerEventType::kHibernateTimeout,
              getNowNs() + kDefaultWakeUpTimeInPowerSaving);
}

void VariableRefreshRateController::threadBody() {
    struct sched_param param = {.sched_priority = 2};
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        LOG(ERROR) << "VrrController: fail to set scheduler to SCHED_FIFO.";
        return;
    }
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mThreadExit) break;
            if (!mEnabled) mCondition.wait(lock);
            if (!mEnabled) continue;

            if (mEventQueue.empty()) {
                mCondition.wait(lock);
            }
            int64_t whenNs = getNextEventTimeLocked();
            int64_t nowNs = getNowNs();
            if (whenNs > nowNs) {
                int64_t delayNs = whenNs - nowNs;
                auto res = mCondition.wait_for(lock, std::chrono::nanoseconds(delayNs));
                if (res != std::cv_status::timeout) {
                    continue;
                }
            }
            if (mEventQueue.empty()) {
                LOG(ERROR) << "VrrController: event queue should NOT be empty.";
                break;
            }
            const auto event = mEventQueue.top();
            mEventQueue.pop();
            LOG(INFO) << "VrrController: handle event in state = " << getStateName(mState)
                      << ", event type = " << event.getName();
            if (mState == VrrControllerState::kRendering) {
                if (event.mEventType == VrrControllerEventType::kHibernateTimeout) {
                    LOG(ERROR) << "VrrController: receiving a hibernate timeout event while in the "
                                  "rendering state.";
                }
                switch (event.mEventType) {
                    case VrrControllerEventType::kRenderingTimeout: {
                        handleHibernate();
                        mState = VrrControllerState::kHibernate;
                        break;
                    }
                    case VrrControllerEventType::kNotifyExpectedPresentConfig: {
                        handleCadenceChange();
                        break;
                    }
                    default: {
                        break;
                    }
                }
            } else {
                if (event.mEventType == VrrControllerEventType::kRenderingTimeout) {
                    LOG(ERROR) << "VrrController: receiving a rendering timeout event while in the "
                                  "hibernate state.";
                }
                if (mState != VrrControllerState::kHibernate) {
                    LOG(ERROR) << "VrrController: expecting to be in hibernate, but instead in "
                                  "state = "
                               << getStateName(mState);
                }
                switch (event.mEventType) {
                    case VrrControllerEventType::kHibernateTimeout: {
                        handleStayHibernate();
                        break;
                    }
                    case VrrControllerEventType::kNotifyExpectedPresentConfig: {
                        handleResume();
                        mState = VrrControllerState::kRendering;
                        break;
                    }
                    case VrrControllerEventType::kNextFrameInsertion: {
                        doFrameInsertionLocked();
                        break;
                    }
                    default: {
                        break;
                    }
                }
            }
        }
    }
}

void VariableRefreshRateController::postEvent(VrrControllerEventType type, int64_t when) {
    VrrControllerEvent event;
    event.mEventType = type;
    event.mWhenNs = when;
    mEventQueue.emplace(event);
}

} // namespace android::hardware::graphics::composer