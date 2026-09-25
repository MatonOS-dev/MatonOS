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

#include <audio_utils/CommandThread.h>

#include <gtest/gtest.h>

TEST(commandthread, basic) {
    android::audio_utils::CommandThread ct;

    ct.add("one", [](){});
    ct.add("two", [](){});
    ct.quit();
    EXPECT_EQ(0, ct.size());
    EXPECT_EQ("", ct.dump());
}

TEST(commandthread, full) {
    std::mutex m;
    std::condition_variable cv;
    int stage = 0;
    android::audio_utils::CommandThread ct;

    // load the CommandThread queue.
    ct.add("one", [&]{
        std::unique_lock ul(m);
        stage = 1;
        cv.notify_one();
        cv.wait(ul, [&] { return stage == 2; });
    });
    ct.add("two", [&]{
        std::unique_lock ul(m);
        stage = 3;
        cv.notify_one();
        cv.wait(ul, [&] { return stage == 4; });
    });
    ct.add("three", [&]{
        std::unique_lock ul(m);
        stage = 5;
        cv.notify_one();
        cv.wait(ul, [&] { return stage == 6; });
    });

    std::unique_lock ul(m);

    // step through each command in the queue.

    cv.wait(ul, [&] { return stage == 1; });
    EXPECT_EQ(2, ct.size());
    EXPECT_EQ("two\nthree\n", ct.dump());
    stage = 2;
    cv.notify_one();

    cv.wait(ul, [&] { return stage == 3; });
    EXPECT_EQ(1, ct.size());
    EXPECT_EQ("three\n", ct.dump());
    stage = 4;
    cv.notify_one();

    cv.wait(ul, [&] { return stage == 5; });
    EXPECT_EQ(0, ct.size());
    EXPECT_EQ("", ct.dump());
    stage = 6;
    cv.notify_one();
}

TEST(commandthread, priority) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    int priority = -1;

    // we only reduce priority - some hosts require permission
    // to raise priority.
    constexpr int kPriority = 130;
    android::audio_utils::CommandThread ct(kPriority);
    ct.add("priority", [&]{
        std::lock_guard lg(m);
        priority = android::audio_utils::get_thread_priority(
                android::audio_utils::gettid_wrapper());
        done = true;
        cv.notify_one();
    });

    std::unique_lock ul(m);
    cv.wait(ul, [&] { return done; });
    EXPECT_EQ(kPriority, priority);
}

static constexpr std::chrono::milliseconds kFirstScheduledTime{40};
static constexpr std::chrono::milliseconds kSecondScheduledTime{80};
static constexpr std::chrono::milliseconds kScheduledFinishTime{500};

TEST(commandthread, scheduled) {
    using namespace std::chrono_literals;
    std::mutex m;
    std::condition_variable cv;
    int stage = 0;
    android::audio_utils::CommandThread ct;

    const auto start = std::chrono::steady_clock::now();
    ct.add("one", [&]{
        std::lock_guard lg(m);
        stage = 1;
        cv.notify_one();
    }, kFirstScheduledTime);

    std::unique_lock ul(m);
    EXPECT_TRUE(cv.wait_for(ul, kScheduledFinishTime, [&] { return stage == 1; }));
    const auto end = std::chrono::steady_clock::now();
    EXPECT_GE(end - start, kFirstScheduledTime);
}

TEST(commandthread, scheduled_multiple) {
    using namespace std::chrono_literals;
    std::mutex m;
    std::condition_variable cv;
    std::vector<int> results;
    android::audio_utils::CommandThread ct;

    // We add them out of order in time.
    ct.add("two", [&]{
        std::lock_guard lg(m);
        results.push_back(2);
        cv.notify_one();
    }, kSecondScheduledTime);

    ct.add("one", [&]{
        std::lock_guard lg(m);
        results.push_back(1);
        cv.notify_one();
    }, kFirstScheduledTime);

    ct.add("zero", [&]{
        std::lock_guard lg(m);
        results.push_back(0);
        cv.notify_one();
    });

    std::unique_lock ul(m);
    EXPECT_TRUE(cv.wait_for(ul, kScheduledFinishTime, [&] { return results.size() == 3; }));
    std::vector<int> expected = {0, 1, 2};
    EXPECT_EQ(expected, results);
}

TEST(commandthread, latest_scheduled_time) {
    using namespace std::chrono_literals;
    android::audio_utils::CommandThread ct;

    EXPECT_FALSE(ct.latest_scheduled_time().has_value());

    const auto now = std::chrono::steady_clock::now();
    ct.add("one", [](){}, kFirstScheduledTime);
    auto latest = ct.latest_scheduled_time();
    EXPECT_TRUE(latest.has_value());
    EXPECT_GE(*latest, now + kFirstScheduledTime);

    ct.add("two", [](){}, kSecondScheduledTime);
    latest = ct.latest_scheduled_time();
    EXPECT_TRUE(latest.has_value());
    EXPECT_GE(*latest, now + kSecondScheduledTime);

    ct.add("zero", [](){}); // Immediate task doesn't affect latest_scheduled_time
    latest = ct.latest_scheduled_time();
    EXPECT_TRUE(latest.has_value());
    EXPECT_GE(*latest, now + kSecondScheduledTime);

    ct.quit();
    EXPECT_FALSE(ct.latest_scheduled_time().has_value());
}

TEST(commandthread, wait_for_all_tasks) {
    using namespace std::chrono_literals;
    android::audio_utils::CommandThread ct;
    std::atomic<int> count = 0;

    ct.add("one", [&]{
        std::this_thread::sleep_for(kFirstScheduledTime);
        count++;
    });
    ct.add("two", [&]{
        std::this_thread::sleep_for(kFirstScheduledTime);
        count++;
    }, kFirstScheduledTime + kFirstScheduledTime);

    ct.wait_for_all_tasks();
    EXPECT_EQ(2, count);
}

TEST(commandthread, quit_add) {
    using namespace std::chrono_literals;
    android::audio_utils::CommandThread ct;

    EXPECT_TRUE(ct.add("one", [](){}));
    EXPECT_TRUE(ct.add("two", [](){}, 10ms));

    ct.quit();

    EXPECT_FALSE(ct.add("three", [](){}));
    EXPECT_FALSE(ct.add("four", [](){}, 10ms));
}
