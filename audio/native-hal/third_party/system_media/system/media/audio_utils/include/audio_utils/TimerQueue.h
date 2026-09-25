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

#pragma once

#include <android-base/thread_annotations.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utils/Errors.h>
#include <utils/Timers.h> // For nsecs_t

namespace android::audio_utils {

/**
 * An interface for a clock that can create and wait for timers.
 * This is used to abstract away the Linux-specific timerfd and epoll logic
 * for testing.
 */
class IClock {
public:
    /**
     * A handle to a timer or poll instance.
     */
    using Handle = int;

    // like POSIX file descriptors, valid handles are non-negative.
    static constexpr Handle INVALID_HANDLE = -1;  // invalid
    static constexpr Handle PENDING_HANDLE = -2;  // operation still pending, retry later
    static constexpr Handle INTR_HANDLE = -3;     // interrupt occurred, retry immediate

    // timer value of 1 nanosecond fires immediately and unblocks the read.
    static constexpr nsecs_t kMagicUnblockTime = 1;

  /**
     * The type of clock to use for a timer.
     */
    enum ClockType {
        BOOTTIME,        // time in nanos since device boot, will not wake from suspend.
        BOOTTIME_ALARM,  // time in nanos since device boot, will wake from suspend.
    };

    virtual ~IClock() = default;

    /**
     * Creates a new timer.
     * @param clockType The type of clock to use.
     * @return A handle to the new timer, or INVALID_HANDLE on error.
     */
    virtual Handle createTimer(ClockType clockType) = 0;

    /**
     * Destroys a timer.
     * @param handle The handle of the timer to destroy.
     * @return OK on success, or a negative error code.
     */
    virtual status_t destroyTimer(Handle handle) = 0;

    /**
     * Returns true if the clock has been successfully initialized and is ready to be used.
     */
    virtual bool ready() const = 0;

    /**
     * Sets a one-shot timer.
     * @param handle The handle of the timer to set.
     * @param time The absolute time in nanoseconds for the timer to expire.
     *             A time of 0 disables the timer.
     * @return OK on success, or a negative error code.
     */
    virtual status_t setTimer(Handle handle, nsecs_t time) = 0;

    /**
     * Waits for a timer to expire.
     * @param timeout The maximum time to wait in nanoseconds. A timeout of -1
     *                  waits forever.
     * @return The handle of the expired timer, PENDING_HANDLE on timeout,
     *         INTR_HANDLE on system interrupt, or INVALID_HANDLE on error.
     */
    virtual Handle wait(nsecs_t timeout) = 0;

    /**
     * Returns a string representation of the clock's internal state,
     * including all active timers.
     *
     * This is useful for debugging.
     *
     * @param prefix A string to prepend to each line of the output.
     * @return A string detailing the current state of the clock.
     */
    virtual std::string toString(std::string_view prefix = {}) const = 0;

    /**
     * Returns a string representation of a specific timer's state.
     *
     * This is useful for debugging.
     *
     * @param prefix A string to prepend to each line of the output.
     * @param handle The handle of the timer to describe.
     * @return A string detailing the current state of the specified timer.
     */
    virtual std::string toString(std::string_view prefix, Handle handle) const = 0;

    /**
     * Creates a new LinuxClock instance.
     */
    static std::unique_ptr<IClock> createLinuxClock();
};

/**
 * A timed execution queue for scheduling functions to run at specific times.
 *
 * TimerQueue allows scheduling of `std::function<void()>` objects to be
 * executed at a future time, based on the `CLOCK_BOOTTIME` clock. It uses a
 * dedicated thread and `timerfd` for efficient and accurate scheduling.
 *
 * Callbacks are executed on the internal TimerQueue thread. If a callback
 * blocks, it will delay the execution of subsequent events.
 *
 * This class is thread-safe.
 */
class TimerQueue {
public:
    /**
     * TimerQueue ctor
     *
     * @param alarm set to true to allow a wake from suspend.
     */
    explicit TimerQueue(bool alarm = false);

    /**
     * TimerQueue ctor for testing.
     *
     * @param clock A clock interface for dependency injection.
     * @param alarm set to true to allow a wake from suspend.
     */
    TimerQueue(std::unique_ptr<IClock> clock, bool alarm);

    ~TimerQueue();

    /**
     * Opaque handle for a pending event.
     *
     * This handle is returned by `add()` and can be used with `remove()` to
     * cancel a pending event before it has executed.
     */
    using EventId = int64_t;
    static constexpr EventId INVALID_EVENT_ID = -1;

    // remove later
    using handle_t = EventId;
    static constexpr auto INVALID_HANDLE = INVALID_EVENT_ID;

    /**
     * Schedules a function to be executed at a specific time.
     *
     * @param function The `std::function<void()>` to execute. The function
     *                 should not be null.
     * @param executionTime The absolute time in nanoseconds, based on the
     *                      `CLOCK_BOOTTIME` monotonic clock, at which the
     *                      function should be executed.
     *
     * @return A unique `EventId` that can be used to cancel the event with
     *         `remove()`. Returns `INVALID_EVENT_ID` if the provided function
     *         is null.
     */
    EventId add(std::function<void()> function, nsecs_t executionTime);

    /**
     * Schedules a function to be executed between softDeadline and hardDeadline.
     *
     * The softDeadline will not trigger if suspended.  The hardDeadline will
     * trigger if suspended.
     *
     * The TimerQueue must be initialized with alarm true for the hardDeadline
     * to work properly, otherwise softDeadline is only used.
     *
     * @param function The `std::function<void()>` to execute. The function
     *                 should not be null.
     * @param softDeadline The earliest absolute time in nanoseconds, based on the
     *                     `CLOCK_BOOTTIME` monotonic clock, at which the
     *                     function should be executed.
     * @param hardDeadline The latest absolute time in nanoseconds, based on the
     *                     `CLOCK_BOOTTIME` monotonic clock, at which the
     *                     function should be executed.  This is ignored if
     *                     mAlarm is false.
     * @param priorityTime The time used to determine which task is scheduled
     *                     if multiple tasks can be executed.  Defaults to the
     *                     hardDeadline if unspecified (or negative).
     *
     * @return A unique `EventId` that can be used to cancel the event with
     *         `remove()`. Returns `INVALID_EVENT_ID` if the provided function
     *         is null.
     */
    EventId add(std::function<void()> function,
        nsecs_t softDeadline, nsecs_t hardDeadline, nsecs_t priorityTime = -1);

    /**
     * Removes a pending function from the execution queue.
     *
     * If the event corresponding to the handle has not yet been executed, it
     * will be removed and will not be called. If the event has already started
     * executing or has finished, this method does nothing.
     *
     * @param id The id of the event to remove, as returned by `add()`.
     *
     * @return `true` if the event was successfully found and removed before
     *         execution, `false` otherwise.
     */
    bool remove(EventId id);

    /**
     * Returns true if the timer was successfully initialized.
     */
    bool ready() const { return mClock->ready(); }

    /**
     * Returns true if the timer can wake from suspend.
     */
    bool alarm() const { return mAlarm; }

    /**
     * Returns a string representation of the TimerQueue's internal state,
     * including all pending events and clock information.
     *
     * This is useful for debugging.
     *
     * @param prefix A string to prepend to each line of the output.
     * @return A string detailing the current state of the TimerQueue.
     */
    std::string toString(std::string_view prefix = {}) const;

private:
    EventId getNextEventId_l() REQUIRES(mMutex);
    void threadLoop();

    struct Event {
        EventId id;
        std::function<void()> function;
        nsecs_t priorityTime; // The time used to determine which task is scheduled
                              // if multiple tasks can be executed.  Defaults to the
                              // hardDeadline if unspecified (or negative).

        // for debugging purposes:
        nsecs_t hardDeadline; // The hard deadline is when the function must be executed.
                              // A soft deadline (not saved) is when the function
                              // can be executed earlier, if the system is not suspended.
        nsecs_t creationTime = systemTime(SYSTEM_TIME_BOOTTIME);

        std::string toString(std::string_view prefix = {}) const {
            std::string s(prefix);
            s.append("id: ").append(std::to_string(id));
            s.append("  priorityTime: ").append(std::to_string(priorityTime));
            s.append("  hardDeadline: ").append(std::to_string(hardDeadline));
            s.append("  creationTime: ").append(std::to_string(creationTime));
            s.append("\n");
            return s;
        }
    };

    // AlarmClock is not inherently thread-safe, but accessed with ThreadQueue mMutex held.
    class AlarmClock {
    public:
        AlarmClock(
                std::string_view name, IClock* clock, IClock::ClockType clockType, bool& running);
        ~AlarmClock();

        IClock::Handle getHandle() const { return mTimerHandle; }
        void add(nsecs_t executionTime, const std::shared_ptr<Event>& event);
        bool remove(EventId id);
        void armTimerForNextEvent();
        void collectEvents(nsecs_t now, std::set<std::shared_ptr<Event>>& events);
        void removeEvents(const std::set<std::shared_ptr<Event>>& events);
        std::string toString(std::string_view prefix = {}) const;
    private:
        const std::string mName;
        IClock* const mClock;
        const IClock::Handle mTimerHandle;
        bool& mRunning;  // from the outer object always read safely.

        // Main storage for event data, keyed by id.
        std::map<EventId, std::pair<std::shared_ptr<Event>, nsecs_t>> mEvents;

        // Index to keep events sorted by time for efficient lookup of the next event.
        std::multimap <nsecs_t, EventId> mTimeIndex;
    };

    std::unique_ptr<IClock> mClock;
    const bool mAlarm;  // if true, in ALARM mode and will wake from suspend.
    mutable std::mutex mMutex;
    std::thread mThread;  // effectively const
    bool mRunning GUARDED_BY(mMutex) = false;
    EventId mNextEventId GUARDED_BY(mMutex) = 1;
    std::vector<AlarmClock> mAlarmClocks GUARDED_BY(mMutex);
};

} // namespace android::audio_utils
