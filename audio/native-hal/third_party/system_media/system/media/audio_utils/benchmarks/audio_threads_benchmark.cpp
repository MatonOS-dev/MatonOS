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

#include <audio_utils/threads.h>
#include <benchmark/benchmark.h>
#include <condition_variable>
#include <mutex>
#include <thread>

/*
Pixel 10
arm64-v8a audio_threads_benchmark
---------------------------------
arm64-v8a audio_threads_benchmark (11 Tests)
[1/11] audio_threads_benchmark#BM_GetTid: PASSED (12ms)
	cpu_time_ns: 1.736073844600092
	family_index: 0
	per_family_instance_index: 0
	real_time_ns: 1.7387132566283472
[2/11] audio_threads_benchmark#BM_SetThreadPriority: PASSED (1ms)
	cpu_time_ns: 627.1550219482076
	family_index: 1
	per_family_instance_index: 0
	real_time_ns: 628.1885040065165
[3/11] audio_threads_benchmark#BM_GetCpu: PASSED (1ms)
	cpu_time_ns: 215.87767003696587
	family_index: 2
	per_family_instance_index: 0
	real_time_ns: 216.23887711897615
[4/11] audio_threads_benchmark#BM_GetNumberCpus: PASSED (1ms)
	cpu_time_ns: 1.6696178552361929
	family_index: 3
	per_family_instance_index: 0
	real_time_ns: 1.672286884951439
[5/11] audio_threads_benchmark#BM_GetThreadPriority: PASSED (0ms)
	cpu_time_ns: 457.2946237725178
	family_index: 4
	per_family_instance_index: 0
	real_time_ns: 458.0852195226619
[6/11] audio_threads_benchmark#BM_GetThreadAffinity: PASSED (1ms)
	cpu_time_ns: 265.01348337715274
	family_index: 5
	per_family_instance_index: 0
	real_time_ns: 265.4066443043617
[7/11] audio_threads_benchmark#BM_NiceToUnifiedPriority: PASSED (1ms)
	cpu_time_ns: 2.6210949141476005
	family_index: 6
	per_family_instance_index: 0
	real_time_ns: 2.625629326077641
[8/11] audio_threads_benchmark#BM_UnifiedPriorityToNice: PASSED (0ms)
	cpu_time_ns: 2.2935873854709645
	family_index: 7
	per_family_instance_index: 0
	real_time_ns: 2.297218932652565
[9/11] audio_threads_benchmark#BM_RtprioToUnifiedPriority: PASSED (0ms)
	cpu_time_ns: 3.6042322969597302
	family_index: 8
	per_family_instance_index: 0
	real_time_ns: 3.6095868751492968
[10/11] audio_threads_benchmark#BM_UnifiedPriorityToRtprio: PASSED (1ms)
	cpu_time_ns: 2.7398383625843916
	family_index: 9
	per_family_instance_index: 0
	real_time_ns: 2.7443589189370305
[11/11] audio_threads_benchmark#BM_IsRealtimePriority: PASSED (1ms)
	cpu_time_ns: 3.276556901513344
	family_index: 10
	per_family_instance_index: 0
	real_time_ns: 3.2816205922208286
*/

using namespace android::audio_utils;

static void BM_GetTid(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(gettid_wrapper());
    }
}
BENCHMARK(BM_GetTid);

static void BM_SetThreadPriority(benchmark::State& state) {
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    bool done = false;
    pid_t thread_tid = 0;

    std::thread t([&]() {
        std::unique_lock<std::mutex> ul(m);
        thread_tid = gettid_wrapper();
        ready = true;
        cv.notify_one();
        cv.wait(ul, [&] { return done; });
    });

    // wait for thread_tid to become valid.
    std::unique_lock<std::mutex> ul(m);
    cv.wait(ul, [&] { return ready; });

    // run benchmark.
    int priority = kDefaultPrio;
    for (auto _ : state) {
        // We use CFS priorities to avoid requiring root/CAP_SYS_NICE for the benchmark.
        benchmark::DoNotOptimize(set_thread_priority(thread_tid, priority));
        priority = (priority == kDefaultPrio) ? kDefaultPrio + 1 : kDefaultPrio;
    }
    done = true;
    ul.unlock();

    // exit thread.
    cv.notify_one();
    t.join();
}
BENCHMARK(BM_SetThreadPriority);

static void BM_GetCpu(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(get_cpu());
    }
}
BENCHMARK(BM_GetCpu);

static void BM_GetNumberCpus(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(get_number_cpus());
    }
}
BENCHMARK(BM_GetNumberCpus);

static void BM_GetThreadPriority(benchmark::State& state) {
    const pid_t tid = gettid_wrapper();
    for (auto _ : state) {
        benchmark::DoNotOptimize(get_thread_priority(tid));
    }
}
BENCHMARK(BM_GetThreadPriority);

static void BM_GetThreadAffinity(benchmark::State& state) {
    const pid_t tid = gettid_wrapper();
    for (auto _ : state) {
        benchmark::DoNotOptimize(get_thread_affinity(tid));
    }
}
BENCHMARK(BM_GetThreadAffinity);

// Conversion functions (mostly inline)
static void BM_NiceToUnifiedPriority(benchmark::State& state) {
    int nice = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(nice_to_unified_priority(nice));
        nice = (nice + 1) % 20;
    }
}
BENCHMARK(BM_NiceToUnifiedPriority);

static void BM_UnifiedPriorityToNice(benchmark::State& state) {
    int priority = kMaxRtPrio;
    for (auto _ : state) {
        benchmark::DoNotOptimize(unified_priority_to_nice(priority));
        priority = kMaxRtPrio + (priority + 1) % kNiceWidth;
    }
}
BENCHMARK(BM_UnifiedPriorityToNice);

static void BM_RtprioToUnifiedPriority(benchmark::State& state) {
    int rtprio = kMinRtPrio;
    for (auto _ : state) {
        benchmark::DoNotOptimize(rtprio_to_unified_priority(rtprio));
        rtprio = kMinRtPrio + (rtprio + 1) % (kMaxRtPrio - kMinRtPrio);
    }
}
BENCHMARK(BM_RtprioToUnifiedPriority);

static void BM_UnifiedPriorityToRtprio(benchmark::State& state) {
    int priority = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(unified_priority_to_rtprio(priority));
        priority = (priority + 1) % kMaxRtPrio;
    }
}
BENCHMARK(BM_UnifiedPriorityToRtprio);

static void BM_IsRealtimePriority(benchmark::State& state) {
    int priority = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(is_realtime_priority(priority));
        priority = (priority + 1) % kMaxPrio;
    }
}
BENCHMARK(BM_IsRealtimePriority);

BENCHMARK_MAIN();
