/*
 * Copyright (C) 2026 The Android Open Source Project
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
#include <benchmark/benchmark.h>
#include <condition_variable>
#include <mutex>

/*
Pixel 10
arm64-v8a audio_command_thread_benchmark
----------------------------------------
arm64-v8a audio_command_thread_benchmark (6 Tests)
[1/6] audio_command_thread_benchmark#BM_CommandThread_Add: PASSED (8ms)
	cpu_time_ns: 3793.3297164381406
	family_index: 0
	per_family_instance_index: 0
	real_time_ns: 3966.720497784497
[2/6] audio_command_thread_benchmark#BM_CommandThread_AddWithCaptures: PASSED (0ms)
	cpu_time_ns: 4985.900870818868
	family_index: 1
	per_family_instance_index: 0
	real_time_ns: 5951.300312368699
[3/6] audio_command_thread_benchmark#BM_CommandThread_Latency: PASSED (1ms)
	cpu_time_ns: 185558.1802897051
	family_index: 2
	per_family_instance_index: 0
	real_time_ns: 292089.0794103293
[4/6] audio_command_thread_benchmark#BM_CommandThread_ScheduleLatency: PASSED (1ms)
	cpu_time_ns: 169660.55197480012
	family_index: 3
	per_family_instance_index: 0
	real_time_ns: 1346338.4242791338
[5/6] audio_command_thread_benchmark#BM_CommandThread_WaitLatency: PASSED (1ms)
	cpu_time_ns: 16.719430401215096
	family_index: 4
	per_family_instance_index: 0
	real_time_ns: 16.741761643209653
[6/6] audio_command_thread_benchmark#BM_StdThread_Latency: PASSED (1ms)
	cpu_time_ns: 580993.5176056337
	family_index: 5
	per_family_instance_index: 0
	real_time_ns: 1492275.5838027333
*/

using namespace android::audio_utils;

/**
 * BM_CommandThread_Add measures the overhead of adding a task to the CommandThread
 * queue from the caller's perspective. This includes mutex acquisition and
 * pushing to the internal deque.
 */
static void BM_CommandThread_Add(benchmark::State& state) {
    CommandThread commandThread;
    for (auto _ : state) {
        commandThread.add("task", []() {});
    }
    commandThread.quit();
}
BENCHMARK(BM_CommandThread_Add);

/**
 * BM_CommandThread_AddWithCaptures measures the overhead of adding a task
 * that has a larger capture sub-object, which may trigger a heap allocation
 * for the std::function.
 */
static void BM_CommandThread_AddWithCaptures(benchmark::State& state) {
    CommandThread commandThread;
    struct Large {
        int data[16];
    } large;
    for (auto _ : state) {
        commandThread.add("task", [large]() {
            int data = large.data[0];
            benchmark::DoNotOptimize(data);
        });
    }
    commandThread.quit();
}
BENCHMARK(BM_CommandThread_AddWithCaptures);

/**
 * BM_CommandThread_Latency measures the round-trip time from adding a task
 * to its completion on the worker thread. This includes thread wake-up and
 * context switching overhead.
 */
static void BM_CommandThread_Latency(benchmark::State& state) {
    CommandThread commandThread;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    for (auto _ : state) {
        {
            std::lock_guard lg(m);
            done = false;
        }
        commandThread.add("task", [&]() {
            std::lock_guard lg(m);
            done = true;
            cv.notify_one();
        });
        std::unique_lock ul(m);
        cv.wait(ul, [&]() { return done; });
    }
    commandThread.quit();
}
BENCHMARK(BM_CommandThread_Latency);

/**
 * BM_CommandThread_ScheduleLatency measures the round-trip time from
 * scheduling a task with a small delay to its completion.
 */
static void BM_CommandThread_ScheduleLatency(benchmark::State& state) {
    using namespace std::chrono_literals;
    CommandThread commandThread;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    for (auto _ : state) {
        {
            std::lock_guard lg(m);
            done = false;
        }
        commandThread.add("task", [&]() {
            std::lock_guard lg(m);
            done = true;
            cv.notify_one();
        }, 1ms);
        std::unique_lock ul(m);
        cv.wait(ul, [&]() { return done; });
    }
    commandThread.quit();
}
BENCHMARK(BM_CommandThread_ScheduleLatency);

/**
 * BM_CommandThread_WaitLatency measures the overhead of calling
 * wait_for_all_tasks() when the queue is already empty.
 */
static void BM_CommandThread_WaitLatency(benchmark::State& state) {
    CommandThread commandThread;
    for (auto _ : state) {
        commandThread.wait_for_all_tasks();
    }
    commandThread.quit();
}
BENCHMARK(BM_CommandThread_WaitLatency);

/**
 * BM_StdThread_Latency measures the round-trip time from creating a new
 * detached thread to its completion. This is used as a baseline to compare
 * with CommandThread's persistent worker thread.
 */
static void BM_StdThread_Latency(benchmark::State& state) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    for (auto _ : state) {
        {
            std::lock_guard lg(m);
            done = false;
        }
        std::thread([&]() {
            std::lock_guard lg(m);
            done = true;
            cv.notify_one();
        }).detach();
        std::unique_lock ul(m);
        cv.wait(ul, [&]() { return done; });
    }
}
BENCHMARK(BM_StdThread_Latency);

BENCHMARK_MAIN();
