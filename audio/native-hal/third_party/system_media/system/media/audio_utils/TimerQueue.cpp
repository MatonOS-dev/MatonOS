/*
 * Copyright (C) 2025 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "TimerQueue"

#include <audio_utils/TimerQueue.h>

// go/keep-sorted start
#include <algorithm>
#include <audio_utils/Statistics.h>
#include <audio_utils/clock.h>
#include <audio_utils/threads.h>
#include <log/log.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <system/thread_defs.h>
#include <unistd.h>
#include <utils/SystemClock.h>
// go/keep-sorted end

namespace android::audio_utils {

static constexpr const char * kIndentPrefix = "  ";

class LinuxClock : public IClock {
public:
    LinuxClock()
        : mPollHandle(epoll_create1(EPOLL_CLOEXEC)) {
        ALOGE_IF(mPollHandle < 0,
                "%s: failed epoll_create1(): %s",
                __func__, strerror(errno));
    }
    ~LinuxClock() override {
        if (mPollHandle != INVALID_HANDLE) close(mPollHandle);
        for (const auto& [handle, _] : mHandleInfos) {
            close(handle);
        }
    }

    Handle createTimer(ClockType clockType) override;
    status_t destroyTimer(Handle handle) override;
    bool ready() const override { return mPollHandle != INVALID_HANDLE; }
    int setTimer(Handle handle, nsecs_t time) override;
    Handle wait(nsecs_t timeout) override;
    std::string toString(std::string_view prefix) const override {
        std::string s;
        for (const auto& [handle, info] : mHandleInfos) {
            appendInfo(s, prefix, handle, info);
            s.append("\n");
        }
        return s;
    }
    std::string toString(std::string_view prefix, Handle handle) const override {
        std::string s;
        if (const auto it = mHandleInfos.find(handle);
            it != mHandleInfos.end()) {
            const auto& info = it->second;
            appendInfo(s, prefix, handle, info);
        }
        return s;
    }

protected:
    static constexpr bool mVerboseLog = false;
    static constexpr nsecs_t kDelayedWakeup = 100'000'000;
    const Handle mPollHandle;
    struct HandleInfo {
        nsecs_t lastTime{};
        int64_t delayed{};
        Statistics<double> statistics{0.99};
    };
    std::map<Handle, HandleInfo> mHandleInfos;

    static void appendInfo(
            std::string& s, std::string_view prefix, Handle handle, const HandleInfo& info) {
        s.append(prefix).append("handle: ")
                .append(std::to_string(handle))
                .append(" lastTime: ").append(std::to_string(info.lastTime))
                .append(" delayed: ").append(std::to_string(info.delayed))
                .append(" statistics(ms): ").append(info.statistics.toString());
    }
};

std::unique_ptr<IClock> IClock::createLinuxClock() {
    return std::make_unique<LinuxClock>();
}

IClock::Handle LinuxClock::createTimer(ClockType clockType) {
    if (!ready()) return INVALID_HANDLE;
    clockid_t id;
    switch (clockType) {
    case BOOTTIME:
        id = CLOCK_BOOTTIME;
        break;
    case BOOTTIME_ALARM:
        id = CLOCK_BOOTTIME_ALARM;
        break;
    default:
        ALOGE("%s: invalid clockType %d", __func__, static_cast<int>(clockType));
        return INVALID_HANDLE;
    }
    const int fd = timerfd_create(id, TFD_CLOEXEC);
    // it is not uncommon for this to fail if there is no permission for CLOCK_BOOTTIME_ALARM.
    if (fd == -1) {
        ALOGE("%s: failed timerfd_create(%d): %s", __func__, id, strerror(errno));
        return INVALID_HANDLE;
    }

    struct epoll_event event {
        .events = EPOLLIN | EPOLLWAKEUP,
        .data = {.fd = fd},
    };
    const int status = epoll_ctl(mPollHandle, EPOLL_CTL_ADD, fd, &event);
    if (status < 0) {
        ALOGE("%s: failed epoll_ctl(): %s", __func__, strerror(errno));
        close(fd);
        return INVALID_HANDLE;
    }
    mHandleInfos.try_emplace(fd, HandleInfo{});
    return fd;
}

status_t LinuxClock::destroyTimer(Handle handle) {
    if (mHandleInfos.erase(handle) == 0) return BAD_VALUE;
    const int status = epoll_ctl(mPollHandle, EPOLL_CTL_DEL, handle, nullptr /* event */);
    return status == 0 ? OK : -errno;
}

status_t LinuxClock::setTimer(Handle handle, nsecs_t time) {
    if (!ready()) return INVALID_HANDLE;
    struct itimerspec spec{};
    if (time > 0) {
        spec.it_value.tv_sec = time / 1'000'000'000;
        spec.it_value.tv_nsec = time % 1'000'000'000;
        mHandleInfos[handle].lastTime = time;
    } else {
        mHandleInfos[handle].lastTime = kMagicUnblockTime;  // do not log.
    }
    const int ret = timerfd_settime(handle, TFD_TIMER_ABSTIME, &spec, nullptr);
    if (ret == 0) return OK;
    return -errno;
}

IClock::Handle LinuxClock::wait(nsecs_t timeout) {
    if (!ready()) return INVALID_HANDLE;
    struct epoll_event event;
    int timeoutMs = (timeout > INT_MAX * 1'000'000LL) ? INT_MAX :
            (timeout < 0) ? -1 :
            timeout / 1'000'000;
    const int n = epoll_wait(mPollHandle, &event, 1 /* maxevents */, timeoutMs);
    if (n < 0) {
        ALOGE_IF(errno != EINTR,
                "%s: wait from poll handle %d failed: %s",
                __func__, mPollHandle, strerror(errno));
        return errno == EINTR ? INTR_HANDLE : INVALID_HANDLE;
    }
    if (n == 0) {
        return PENDING_HANDLE;
    }

    const int fd = event.data.fd;
    uint64_t expirations;
    ssize_t nread = read(fd, &expirations, sizeof(expirations));
    ALOGV("%s: read %zd from timer %d", __func__, nread, fd);
    if (nread < 0) {
        ALOGE("%s: read from timer %d failed: %s", __func__, fd, strerror(errno));
        if (errno == EAGAIN || errno == EINTR) return PENDING_HANDLE;
        return INVALID_HANDLE;
    }
    const nsecs_t now = elapsedRealtimeNano();
    if (mHandleInfos.count(fd) == 0) {
        ALOGE("%s: trigger now %jd when mLastTime for fd %d doesn't exist",
                __func__, now, fd);
    } else {
       auto& handleInfo = mHandleInfos[fd];
       if (const auto lastTime = handleInfo.lastTime;
               lastTime != kMagicUnblockTime) {
            const auto diff = now - lastTime;
            if (diff < 0) {
                ALOGE("%s: trigger now %jd earlier %jd than mLastTime[%d] %jd",
                        __func__, now, diff, fd, lastTime);
            } else if (diff > kDelayedWakeup) {
                ALOGW("%s: trigger now %jd later %jd than mLastTime[%d] %jd (threshold %lld)",
                        __func__, now, diff, fd, lastTime, (long long)kDelayedWakeup);
                ++handleInfo.delayed;
            } else if (mVerboseLog) {
                ALOGD("%s: trigger now %jd later %jd mLastTime[%d] %jd",
                        __func__, now, diff, fd, lastTime);
            }
            handleInfo.statistics.add(diff * 1e-6); // msec
        }
    }
    return fd;
}

TimerQueue::TimerQueue(bool alarm)
    : TimerQueue(IClock::createLinuxClock(), alarm) {
}

TimerQueue::TimerQueue(std::unique_ptr<IClock> clock, bool alarm)
    : mClock(std::move(clock)),
      mAlarm(alarm) {

    // create our alarm clocks
    mAlarmClocks.emplace_back(
            "BootTime", mClock.get(), IClock::BOOTTIME, mRunning);
    if (alarm) {
        mAlarmClocks.emplace_back(
                "BootTime Alarm", mClock.get(), IClock::BOOTTIME_ALARM, mRunning);
    }
    mRunning = true;
    mThread = std::thread(&TimerQueue::threadLoop, this);
}

TimerQueue::~TimerQueue() {
    if (!mClock->ready()) return;

    {
        std::lock_guard lock(mMutex);
        mRunning = false;
        for (auto& alarmClock : mAlarmClocks) {
            alarmClock.armTimerForNextEvent();
        }
    }

    if (mThread.joinable()) {
        mThread.join();
    }
    mClock.reset();
    mAlarmClocks.clear();
}

TimerQueue::EventId TimerQueue::add(std::function<void()> function, nsecs_t executionTime) {
    if (!mClock->ready() || !function) {
        return INVALID_EVENT_ID;
    }

    std::lock_guard lock(mMutex);
    const EventId id = getNextEventId_l();
    const auto event = std::make_shared<Event>(
            Event{id, std::move(function), executionTime, executionTime});

    if (mAlarm) {
        mAlarmClocks[1].add(executionTime, event);
    } else {
        mAlarmClocks[0].add(executionTime, event);
    }
    return id;
}

TimerQueue::EventId TimerQueue::add(std::function<void()> function,
            nsecs_t softDeadline, nsecs_t hardDeadline, nsecs_t priorityTime) {
    if (!mClock->ready() || !function) {
        return INVALID_EVENT_ID;
    }

    std::lock_guard lock(mMutex);
    const EventId id = getNextEventId_l();
    const auto event = std::make_shared<Event>(Event{id, std::move(function),
            priorityTime >= 0 ? priorityTime : hardDeadline, hardDeadline});

    if (mAlarm) {
        mAlarmClocks[0].add(softDeadline, event);
        mAlarmClocks[1].add(hardDeadline, event);
    } else {
        mAlarmClocks[0].add(softDeadline, event);
    }
    return id;
}


bool TimerQueue::remove(EventId id) {
    if (!mClock->ready() || id == INVALID_EVENT_ID) {
        return false;
    }

    // check all clocks (an id can belong to more than one clock).
    bool found = false;
    std::lock_guard lock(mMutex);
    for (auto& alarmClock : mAlarmClocks) {
        if (alarmClock.remove(id)) found = true;
    }
    return found;
}

TimerQueue::EventId TimerQueue::getNextEventId_l() {
    const EventId id = mNextEventId;
    if (id == std::numeric_limits<EventId>::max()) { // id wrap-around
        mNextEventId = 1;
    } else {
        ++mNextEventId;
    }
    return id;
}

std::string TimerQueue::toString(std::string_view prefix) const {
    std::string s{prefix};
    s.append("TimerQueue Event Queues:\n");
    std::lock_guard lock(mMutex);
    std::string prefix2 = std::string(prefix).append(kIndentPrefix);
    for (size_t i = 0; i < mAlarmClocks.size(); ++i) {
        s.append(prefix).append("[").append(std::to_string(i)).append("] ");
        s.append(mAlarmClocks[i].toString(prefix2));
    }
    s.append(prefix).append("TimerQueue Clock Handles:\n");
    s.append(mClock->toString(prefix2));
    return s;
}

void TimerQueue::threadLoop() {
    constexpr const char* kThreadName = "TimerQueue";
    audio_utils::set_thread_name(kThreadName);
    constexpr int kPriority = audio_utils::nice_to_unified_priority(ANDROID_PRIORITY_AUDIO);
    const status_t status = audio_utils::set_thread_priority(kPriority);
    ALOGW_IF(status != OK, "%s: set priority %d failed with status %d",
             __func__, kPriority, status);
    while (true) {
        const IClock::Handle handle = mClock->wait(-1 /* timeout */);
        ALOGV("%s: Clock wait %d", __func__, handle);

        if (handle == IClock::INVALID_HANDLE) {
            break;
        } else if (handle == IClock::PENDING_HANDLE || handle == IClock::INTR_HANDLE) {
            continue;
        }
        const nsecs_t now = elapsedRealtimeNano();
        std::set<std::shared_ptr<Event>> events;
        {
            std::lock_guard lock(mMutex);

            if (!mRunning) break;

            // collect all the events that are active
            for (auto& alarmClock : mAlarmClocks) {
                alarmClock.collectEvents(now, events);
            }
            // if an event has been registered on multiple alarms, remove it.
            // to prevent duplicate execution.
            for (auto& alarmClock : mAlarmClocks) {
                alarmClock.removeEvents(events);
            }
        }
        std::vector<std::shared_ptr<Event>> sorted{events.begin(), events.end()};
        std::sort(sorted.begin(), sorted.end(),
                [](const std::shared_ptr<Event>& e1, const std::shared_ptr<Event>& e2) {
                    return e1->priorityTime < e2->priorityTime;});
        // execute the lambdas outside the lock
        constexpr int kWarningMs = 100;
        for (const auto& event : sorted) {
            if ((now - event->hardDeadline) > kWarningMs * NANOS_PER_MILLISECOND) {
                ALOGW("%s: Event hard deadline exceeded by %d ms:"
                        " creation: %lld, now: %lld, hardDeadline: %lld",
                        __func__, kWarningMs, (long long)event->creationTime,
                         (long long)now, (long long)event->hardDeadline);
            }
            event->function();
        }
    }
}

TimerQueue::AlarmClock::AlarmClock(std::string_view name,
        IClock* clock, IClock::ClockType clockType, bool& running)
    : mName(name)
    , mClock(clock)
    , mTimerHandle{mClock->createTimer(clockType)}
    , mRunning(running) {
    if (mTimerHandle < 0) {
        ALOGE("%s: failed to create timer for clockType %d: %s",
                __func__, clockType, strerror(errno));
        return;
    }
}

TimerQueue::AlarmClock::~AlarmClock() {
}

void TimerQueue::AlarmClock::add(nsecs_t executionTime, const std::shared_ptr<Event>& event) {

    const bool needsReschedule = mTimeIndex.empty() || executionTime < mTimeIndex.begin()->first;

    mEvents.emplace(event->id, std::make_pair(event, executionTime));
    mTimeIndex.emplace(executionTime, event->id);

    if (needsReschedule) {
        armTimerForNextEvent();
    }
}

bool TimerQueue::AlarmClock::remove(EventId id) {
    if (id == INVALID_EVENT_ID) {
        return false;
    }

    const auto eventIt = mEvents.find(id);
    if (eventIt == mEvents.end()) {
        return false;
    }

    auto [event, executionTime] = eventIt->second;
    const bool wasNext = (mTimeIndex.begin()->second == id);

    mEvents.erase(eventIt);

    const auto [first, last] = mTimeIndex.equal_range(executionTime);
    for (auto it = first; it != last; ++it) {
        if (it->second == id) {
            mTimeIndex.erase(it);
            break;
        }
    }

    if (wasNext) {
        armTimerForNextEvent();
    }

    // caution event may be released inside of lock.
    return true;
}

void TimerQueue::AlarmClock::armTimerForNextEvent() {
    nsecs_t nextTime = 0;
    if (!mRunning) {
        // Set a timer for 1 nanosecond to ensure it fires immediately and unblocks the read.
        nextTime = IClock::kMagicUnblockTime;
    } else if (!mTimeIndex.empty()) {
        nextTime = mTimeIndex.begin()->first;
    }
    mClock->setTimer(mTimerHandle, nextTime);
}

void TimerQueue::AlarmClock::collectEvents(
        nsecs_t now, std::set<std::shared_ptr<Event>>& events) {
    while (!mTimeIndex.empty() && mTimeIndex.begin()->first <= now) {
        const auto it = mTimeIndex.begin();
        const EventId id = it->second;

        const auto eventIt = mEvents.find(id);
        if (eventIt != mEvents.end()) {
            events.emplace(std::move(eventIt->second.first));
            mEvents.erase(eventIt);
        }
        mTimeIndex.erase(it);
    }
    armTimerForNextEvent();
}

void TimerQueue::AlarmClock::removeEvents(const std::set<std::shared_ptr<Event>>& events) {
    for (const auto& event : events) {
        remove(event->id);
    }
}

std::string TimerQueue::AlarmClock::toString(std::string_view prefix) const {
    std::string s{prefix};
    s.append(mName).append("\n");
    if (!mEvents.empty()) {
        s.append(prefix).append("Active Events:\n");
        std::string prefix2 = std::string(prefix).append(kIndentPrefix);
        for (const auto& event: mEvents) {
            s.append(event.second.first->toString(prefix2));
        }
    }
    return s;
}

} // namespace android::audio_utils
