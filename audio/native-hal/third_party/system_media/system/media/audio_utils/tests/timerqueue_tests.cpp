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

#include <audio_utils/TimerQueue.h>

#include <audio_utils/mutex.h>  // audio_utils::unique_lock has thread safety annotations
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <utils/SystemClock.h>

using namespace std::chrono_literals;

namespace android::audio_utils {

class TimerQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::lock_guard lock(mMutex);
        mExecuted = false;
    }

    void TearDown() override {
    }

    std::mutex mMutex;
    std::condition_variable mCv;
    bool mExecuted GUARDED_BY(mMutex);
};

TEST_F(TimerQueueTest, AddAndExecute) {
    TimerQueue tq;

    ASSERT_TRUE(tq.ready());

    const auto executionTime = elapsedRealtimeNano() + 20'000'000; // 20 ms

    tq.add([this]() {
        std::lock_guard lock(mMutex);
        mExecuted = true;
        mCv.notify_one();
    }, executionTime);

    audio_utils::unique_lock ul(mMutex);
    const auto timepoint = std::chrono::steady_clock::now() + 100ms;
    int tries = 0;
    do {
        const auto status = mCv.wait_until(ul, timepoint);
        EXPECT_EQ(std::cv_status::no_timeout, status);
        ++tries;
        ASSERT_LT(tries, 10);
    } while (!mExecuted);
}

TEST_F(TimerQueueTest, Remove) {
    TimerQueue tq;

    ASSERT_TRUE(tq.ready());

    const auto executionTime = elapsedRealtimeNano() + 50'000'000; // 50 ms

    auto id = tq.add([this]() {
        std::lock_guard lock(mMutex);
        mExecuted = true;
        mCv.notify_one();
    }, executionTime);

    ASSERT_NE(id, TimerQueue::INVALID_EVENT_ID);

    bool removed = tq.remove(id);
    ASSERT_TRUE(removed);

    audio_utils::unique_lock ul(mMutex);
    const auto timepoint = std::chrono::steady_clock::now() + 100ms;
    std::cv_status status = std::cv_status::no_timeout;
    do {
        status = mCv.wait_until(ul, timepoint);
        EXPECT_FALSE(mExecuted);
    } while (status == std::cv_status::no_timeout);
    EXPECT_EQ(std::cv_status::timeout, status);
}

TEST_F(TimerQueueTest, MultipleEvents) {
    TimerQueue tq;

    ASSERT_TRUE(tq.ready());

    std::vector<int> executionOrder;
    std::mutex orderMutex;

    const auto t1 = elapsedRealtimeNano() + 40'000'000; // 40ms
    const auto t2 = elapsedRealtimeNano() + 20'000'000; // 20ms
    const auto t3 = elapsedRealtimeNano() + 60'000'000; // 60ms

    tq.add([&]() {
        std::unique_lock<std::mutex> lock(orderMutex);
        executionOrder.push_back(1);
    }, t1);

    tq.add([&]() {
        std::unique_lock<std::mutex> lock(orderMutex);
        executionOrder.push_back(2);
        mCv.notify_one(); // Notify after the first event
    }, t2);

    tq.add([&]() {
        std::unique_lock<std::mutex> lock(orderMutex);
        executionOrder.push_back(3);
    }, t3);

    // Wait for the first event to fire
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mCv.wait_for(lock, 100ms);
    }

    // Wait a bit longer for other events
    std::this_thread::sleep_for(100ms);

    std::unique_lock<std::mutex> lock(orderMutex);
    ASSERT_EQ(3UL, executionOrder.size());
    EXPECT_EQ(2, executionOrder[0]);
    EXPECT_EQ(1, executionOrder[1]);
    EXPECT_EQ(3, executionOrder[2]);
}

TEST_F(TimerQueueTest, Destructor) {
    {
        TimerQueue tq;
        ASSERT_TRUE(tq.ready());

        const auto executionTime = elapsedRealtimeNano() + 50'000'000; // 50 ms
        tq.add([this]() {
            std::lock_guard lock(mMutex);
            mExecuted = true;
        }, executionTime);
    } // tq is destroyed here

    std::this_thread::sleep_for(100ms);
    std::lock_guard lock(mMutex);
    EXPECT_FALSE(mExecuted);
}

TEST_F(TimerQueueTest, RemoveInvalidEventId) {
    TimerQueue tq;
    ASSERT_TRUE(tq.ready());
    EXPECT_FALSE(tq.remove(12345));
    EXPECT_FALSE(tq.remove(TimerQueue::INVALID_EVENT_ID));
}

TEST_F(TimerQueueTest, AddNullFunction) {
    TimerQueue tq;
    ASSERT_TRUE(tq.ready());
    const auto id = tq.add(nullptr, elapsedRealtimeNano() + 10'000'000);
    EXPECT_EQ(id, TimerQueue::INVALID_EVENT_ID);
}

TEST_F(TimerQueueTest, PriorityOrder) {
    TimerQueue tq;

    ASSERT_TRUE(tq.ready());

    std::vector<size_t> executionOrder;

    const auto now = elapsedRealtimeNano();
    const auto deadline = now + 100'000'000; // 100ms
    constexpr size_t kTasks = 8;

    for (size_t priority = kTasks; priority >= 1; --priority) {
        tq.add([&executionOrder, priority, this]() {
            std::lock_guard lg(mMutex);
            executionOrder.push_back(priority);
            if (priority == 1) mCv.notify_one();
        }, deadline, deadline, priority);
    }

    // Wait for the event to fire
    {
        std::unique_lock<std::mutex> ul(mMutex);
        mCv.wait_for(ul, 200ms);
    }

    // Wait a bit longer for other events
    std::this_thread::sleep_for(100ms);

    std::lock_guard lg(mMutex);
    ASSERT_EQ(kTasks, executionOrder.size());
    for (size_t i = 0; i < kTasks; ++i) {
       EXPECT_EQ(i + 1, executionOrder[i]);
    }
}

class IClockTest : public ::testing::Test {
protected:
    void SetUp() override {
        mClock = IClock::createLinuxClock();
    }

    std::unique_ptr<IClock> mClock;
};

TEST_F(IClockTest, CreateAndDestroy) {
    ASSERT_TRUE(mClock->ready());
    auto timer = mClock->createTimer(IClock::BOOTTIME);
    ASSERT_NE(timer, IClock::INVALID_HANDLE);
    ASSERT_EQ(OK, mClock->destroyTimer(timer));
}

TEST_F(IClockTest, SetAndWait) {
    ASSERT_TRUE(mClock->ready());
    auto timer = mClock->createTimer(IClock::BOOTTIME);
    ASSERT_NE(timer, IClock::INVALID_HANDLE);

    const auto executionTime = elapsedRealtimeNano() + 20'000'000; // 20 ms
    ASSERT_EQ(OK, mClock->setTimer(timer, executionTime));

    auto handle = mClock->wait(30'000'000);
    ASSERT_EQ(timer, handle);

    ASSERT_EQ(OK, mClock->destroyTimer(timer));
}

TEST_F(IClockTest, WaitTimeout) {
    ASSERT_TRUE(mClock->ready());
    auto timer = mClock->createTimer(IClock::BOOTTIME);
    ASSERT_NE(timer, IClock::INVALID_HANDLE);

    const auto executionTime = elapsedRealtimeNano() + 50'000'000; // 50 ms
    ASSERT_EQ(OK, mClock->setTimer(timer, executionTime));

    auto handle = mClock->wait(20'000'000);
    ASSERT_EQ(IClock::PENDING_HANDLE, handle);

    ASSERT_EQ(OK, mClock->destroyTimer(timer));
}

} // namespace android::audio_utils
