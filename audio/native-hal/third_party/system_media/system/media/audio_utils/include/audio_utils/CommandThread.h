/*
 * Copyright (C) 2024 The Android Open Source Project
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

// go/keep-sorted start
#include <audio_utils/mutex.h>
#include <audio_utils/threads.h>
#include <utils/Log.h>
// go/keep-sorted end

// go/keep-sorted start
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
// go/keep-sorted end

#pragma push_macro("LOG_TAG")
#undef LOG_TAG
#define LOG_TAG "audio_utils::CommandThread"

namespace android::audio_utils {

/**
 * CommandThread is used for serial execution of commands
 * on a single worker thread.
 *
 * This class is thread-safe.
 */

class CommandThread {
public:
    /**
     * Default constructor for CommandThread.
     * The worker thread is started immediately with default priority.
     *
     * @param name optional name for the worker thread.
     */
    explicit CommandThread(std::string_view name = "") {
        // threadLoop() should be started after the class is initialized.
        mThread = std::thread([this, nameStr = std::string(name)](){
            if (!nameStr.empty()) {
                set_thread_name(nameStr);
            }
            this->threadLoop();
        });
    }

    /**
     * Constructor for CommandThread with a specified priority.
     * The worker thread is started immediately with the given priority.
     *
     * @param priority the unified priority to set for the worker thread.
     */
    explicit CommandThread(int priority) : CommandThread("", priority) {}

    /**
     * Constructor for CommandThread with a specified name and priority.
     * The worker thread is started immediately with the given priority.
     *
     * The linux kernel unified scheduler priority values are as follows:
     * 0 - 98    (A real time priority rtprio between 99 and 1)
     * 100 - 139 (A Completely Fair Scheduler niceness between -20 and 19)
     *
     * A priority value of 99 is changed to 98.
     *
     * Real time schedulers (SCHED_FIFO and SCHED_RR) have rtprio between 1 and 99,
     * where 1 is the lowest and 99 is the highest.
     *
     * The Completely Fair Scheduler (also known as SCHED_OTHER) has a
     * nice value between 19 and -20, where 19 is the lowest and -20 the highest.
     *
     * Note: the unified priority is reported on /proc/<tid>/stat file as "prio".
     *
     * See audio_utils/threads.h for a description of unified priority.
     *
     * @param name name for the worker thread.
     * @param priority the unified priority to set for the worker thread.
     */
    CommandThread(std::string_view name, int priority) {
        // threadLoop() should be started after the class is initialized.
        mThread = std::thread([this, priority, nameStr = std::string(name)](){
            if (!nameStr.empty()) {
                set_thread_name(nameStr);
            }
            const status_t status = set_thread_priority(priority);
            ALOGW_IF(status != OK, "%s: set priority %d failed with status %d",
                    __func__, priority, status);
            this->threadLoop();
        });
    }

    ~CommandThread() {
        quit();
        mThread.join();
    }

    /**
     * Add a command to the command queue.
     *
     * If the func is a closure containing references, suggest using shared_ptr
     * instead to maintain proper lifetime.
     *
     * @param name for dump() purposes.
     * @param func command to execute
     * @return true if successfully added, false if the CommandThread is quitting.
     */
    bool add(std::string_view name, std::function<void()>&& func) {
        std::lock_guard lg(mMutex);
        if (mQuit) return false;
        mCommands.emplace_back(name, std::move(func));
        if (mCommands.size() == 1) mConditionVariable.notify_one();
        return true;
    }

    /**
     * Add a command to the command queue to be executed after a delay.
     *
     * If the func is a closure containing references, suggest using shared_ptr
     * instead of references to maintain proper lifetime.
     *
     * @param name for dump() purposes.
     * @param func command to execute
     * @param delay duration to wait before executing the command
     * @return true if successfully added, false if the CommandThread is quitting.
     */
    bool add(std::string_view name, std::function<void()>&& func,
            std::chrono::nanoseconds delay) {
        std::lock_guard lg(mMutex);
        if (mQuit) return false;
        if (delay <= std::chrono::nanoseconds(0)) {
            mCommands.emplace_back(name, std::move(func));
            if (mCommands.size() == 1) mConditionVariable.notify_one();
        } else {
            const auto wakeupTime = std::chrono::steady_clock::now() + delay;
            const bool earliest = mScheduledCommands.empty() ||
                    wakeupTime < mScheduledCommands.begin()->first;
            mScheduledCommands.emplace(
                    wakeupTime, std::make_pair(std::string(name), std::move(func)));
            if (earliest && mCommands.empty()) mConditionVariable.notify_one();
        }
        return true;
    }

    /**
     * Returns the string of commands, separated by newlines.
     */
    std::string dump() const {
        std::string result;
        std::lock_guard lg(mMutex);
        for (const auto &p : mCommands) {
            result.append(p.first).append("\n");
        }
        for (const auto &p : mScheduledCommands) {
            result.append(p.second.first).append(" (scheduled)\n");
        }
        return result;
    }

    /**
     * Quits the command thread and empties the command queue.
     */
    void quit() {
        std::lock_guard lg(mMutex);
        if (mQuit) return;
        mQuit = true;
        mCommands.clear();
        mScheduledCommands.clear();
        mConditionVariable.notify_one();
        mWaitConditionVariable.notify_all();
    }

    /**
     * Wait for all tasks to complete.
     */
    void wait_for_all_tasks() {
        audio_utils::unique_lock ul(mMutex);
        mWaitConditionVariable.wait(ul, [this]() REQUIRES(mMutex) {
            return (mCommands.empty() && mScheduledCommands.empty() && !mTaskRunning) || mQuit;
        });
    }

    /**
     * Returns the number of commands on the queue.
     */
    size_t size() const {
        std::lock_guard lg(mMutex);
        return mCommands.size() + mScheduledCommands.size();
    }

    /**
     * Returns the time of the last scheduled task in the CommandThread.
     */
    std::optional<std::chrono::steady_clock::time_point> latest_scheduled_time() const {
        std::lock_guard lg(mMutex);
        if (mScheduledCommands.empty()) return std::nullopt;
        return mScheduledCommands.rbegin()->first;
    }

private:
    std::thread mThread;
    mutable std::mutex mMutex;
    std::condition_variable mConditionVariable GUARDED_BY(mMutex);
    std::condition_variable mWaitConditionVariable GUARDED_BY(mMutex);
    std::deque<std::pair<std::string, std::function<void()>>> mCommands GUARDED_BY(mMutex);
    std::multimap<std::chrono::steady_clock::time_point,
            std::pair<std::string, std::function<void()>>> mScheduledCommands GUARDED_BY(mMutex);
    bool mQuit GUARDED_BY(mMutex) = false;
    bool mTaskRunning GUARDED_BY(mMutex) = false;

    void threadLoop() {
        audio_utils::unique_lock ul(mMutex);
        while (!mQuit) {
            const auto now = std::chrono::steady_clock::now();
            // transfer scheduled commands that are ready to the command queue.
            while (!mScheduledCommands.empty() && mScheduledCommands.begin()->first <= now) {
                mCommands.push_back(std::move(mScheduledCommands.begin()->second));
                mScheduledCommands.erase(mScheduledCommands.begin());
            }

            // process the command queue.
            if (!mCommands.empty()) {
                auto name = std::move(mCommands.front().first);
                auto func = std::move(mCommands.front().second);
                mCommands.pop_front();
                mTaskRunning = true;
                ul.unlock();
                // ALOGD("%s: executing %s", __func__, name.c_str());
                func();
                ul.lock();
                mTaskRunning = false;
                mWaitConditionVariable.notify_all();
                continue;
            }

            if (!mScheduledCommands.empty()) {
                mConditionVariable.wait_until(ul, mScheduledCommands.begin()->first);
            } else {
                mWaitConditionVariable.notify_all();
                mConditionVariable.wait(ul);
            }
        }
    }
};

}  // namespace android::audio_utils

#pragma pop_macro("LOG_TAG")
