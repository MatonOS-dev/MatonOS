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

#include <atomic>
#include <audio_utils/futex.h>
#include <benchmark/benchmark.h>

/*
Pixel 10
arm64-v8a audio_futex_benchmark
-------------------------------
arm64-v8a audio_futex_benchmark (3 Tests)
[1/3] audio_futex_benchmark#BM_FutexWakeWait/threads:2: PASSED (13ms)
	cpu_time_ns: 72689.55835837984
	family_index: 0
	per_family_instance_index: 0
	real_time_ns: 121115.17475311892
[2/3] audio_futex_benchmark#BM_FutexWakeNoWaiters: PASSED (1ms)
	cpu_time_ns: 140.52471112885357
	family_index: 1
	per_family_instance_index: 0
	real_time_ns: 140.6651626879838
[3/3] audio_futex_benchmark#BM_FutexWaitAlreadyChanged: PASSED (1ms)
	cpu_time_ns: 151.43058757540177
	family_index: 2
	per_family_instance_index: 0
	real_time_ns: 151.59144450341986
*/


// We set up a futex wait/wake chain of size 2.
static inline constexpr size_t THREADS = 2;

class FutexWakeWait {
    std::atomic<int32_t> futexes[THREADS];

public:
    FutexWakeWait() {
        for (auto& futex : futexes) {
            futex.store(0);
        }
    }

    void run(benchmark::State& state) {
        // Find our futex index.
        const size_t local = state.thread_index();
        // Index of next futex thread in chain.
        const size_t remote = (local + 1) % THREADS;

        if (local == 0) {
            // If we are thread 0, we start the wake/wait chain
            futexes[local].store(1);
        }

        for (auto _ : state) {
            // Wait for our futex to be 1
            while (futexes[local].load() == 0) {
                ::sys_futex(/* addr1 */ static_cast<void*>(&futexes[local]),
                        /* op */ FUTEX_WAIT_PRIVATE, /* val1 */ 0, /* timeout */ nullptr,
                        /* addr2 */ nullptr, /* val3 */ 0);
            }
            // Reset our futex
            futexes[local].store(0);

            // Wake up the next thread
            futexes[remote].store(1);
            ::sys_futex(/* addr1 */ static_cast<void*>(&futexes[remote]),
                    /* op */ FUTEX_WAKE_PRIVATE, /* val1 */ 1, /* timeout */ nullptr,
                    /* addr2 */ nullptr, /* val3 */ 0);
        }

        // Clean up to avoid hanging the next thread on exit
        futexes[remote].store(1);
        ::sys_futex(/* addr1 */ static_cast<void*>(&futexes[remote]),
                /* op */ FUTEX_WAKE_PRIVATE, /* val1 */ 1, /* timeout */ nullptr,
                /* addr2 */ nullptr, /* val3 */ 0);
    }
};


// Singleton benchmark shared by all threads.
static FutexWakeWait gFutexWakeWait;

static void BM_FutexWakeWait(benchmark::State& state) {
    gFutexWakeWait.run(state);
}

BENCHMARK(BM_FutexWakeWait)->Threads(THREADS);

static void BM_FutexWakeNoWaiters(benchmark::State& state) {
    std::atomic<int32_t> value{0};
    for (auto _ : state) {
        ::sys_futex(/* addr1 */ static_cast<void*>(&value),
                /* op */ FUTEX_WAKE_PRIVATE, /* val1 */ 1, /* timeout */ nullptr,
                /* addr2 */ nullptr, /* val3 */ 0);
    }
}
BENCHMARK(BM_FutexWakeNoWaiters);

static void BM_FutexWaitAlreadyChanged(benchmark::State& state) {
    std::atomic<int32_t> value{1};
    for (auto _ : state) {
        // Since futex value (1) != expected value (0), this returns immediately with EWOULDBLOCK.
        ::sys_futex(/* addr1 */ static_cast<void*>(&value),
                /* op */ FUTEX_WAIT_PRIVATE, /* val1 */ 0, /* timeout */ nullptr,
                /* addr2 */ nullptr, /* val3 */ 0);
    }
}
BENCHMARK(BM_FutexWaitAlreadyChanged);

BENCHMARK_MAIN();
