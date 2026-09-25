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

#define LOG_TAG "audio_utils::threads"

#include <audio_utils/threads.h>

#include <algorithm>  // std::clamp
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>    // scheduler
#include <sys/resource.h>
#include <system/thread_defs.h>
#include <thread>
#include <utils/Errors.h>  // status_t
#include <utils/Log.h>

namespace android::audio_utils {

/**
 * Sets the unified priority of the tid.
 */
status_t set_thread_priority(pid_t tid, int priority) {
    if (tid == 0) tid = gettid_wrapper();
    const int policy = sched_getscheduler(tid);
    if (policy < 0) return -errno;
    const int basePolicy = policy & ~SCHED_RESET_ON_FORK;
    const int resetOnFork = policy & SCHED_RESET_ON_FORK; // preserve if possible
    if (is_realtime_priority(priority)) {
        // audio processes are designed to work with FIFO, not RR.
        const int new_policy = SCHED_FIFO | resetOnFork;
        const int rtprio = unified_priority_to_rtprio(priority);
        struct sched_param param {
            .sched_priority = rtprio,
        };
        if (sched_setscheduler(tid, new_policy, &param) != 0) {
            ALOGW("%s: Cannot set FIFO priority for tid %d to policy %d rtprio %d  %s",
                    __func__, tid, new_policy, rtprio, strerror(errno));
            return -errno;
        }
        return NO_ERROR;
    } else if (is_cfs_priority(priority)) {
        const int nice = unified_priority_to_nice(priority);
        if (basePolicy != SCHED_OTHER) {
            struct sched_param param{};
            const int new_policy = SCHED_OTHER | resetOnFork;
            if (sched_setscheduler(tid, new_policy, &param) != 0) {
                ALOGW("%s: Cannot set CFS priority for tid %d "
                        "from policy %d to policy %d nice %d  %s",
                        __func__, tid, policy, new_policy, nice, strerror(errno));
                return -errno;
            }
        }
        if (setpriority(PRIO_PROCESS, tid, nice) != 0) return -errno;
        return NO_ERROR;
    } else {
        return BAD_VALUE;
    }
}

/**
 * Returns the unified priority of the tid.
 *
 * A negative number represents error.
 */
int get_thread_priority(pid_t tid) {
    if (tid == 0) tid = gettid_wrapper();
    int policy = sched_getscheduler(tid);
    if (policy < 0) return -errno;

    policy &= ~SCHED_RESET_ON_FORK; // get base policy
    if (policy == SCHED_OTHER) {
        errno = 0;  // negative return value valid, so check errno change.
        const int nice = getpriority(PRIO_PROCESS, tid);
        if (errno != 0) return -errno;
        return nice_to_unified_priority(nice);
    } else if (policy == SCHED_FIFO || policy == SCHED_RR) {
        struct sched_param param{};
        if (sched_getparam(tid, &param) < 0) return -errno;
        return rtprio_to_unified_priority(param.sched_priority);
    } else {
        return INVALID_OPERATION;
    }
}

status_t set_thread_affinity(pid_t tid, const std::bitset<kMaxCpus>& mask) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    const size_t limit = std::min(get_number_cpus(), kMaxCpus);
    for (size_t i = 0; i < limit; ++i) {
        if (mask.test(i)) {
            CPU_SET(i, &cpuset);
        }
    }
    if (sched_setaffinity(tid, sizeof(cpuset), &cpuset) == 0) {
        return OK;
    }
    return -errno;
}

std::bitset<kMaxCpus> get_thread_affinity(pid_t tid) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    std::bitset<kMaxCpus> mask;
    if (sched_getaffinity(tid, sizeof(cpuset), &cpuset) == 0) {
        const size_t limit = std::min(get_number_cpus(), kMaxCpus);
        for (size_t i = 0; i < limit; ++i) {
            if (CPU_ISSET(i, &cpuset)) {
                mask.set(i);
            }
        }
    }
    return mask;
}

int get_cpu() {
    return sched_getcpu();
}

/*
 * std::thread::hardware_concurrency() is not optimized.  We cache the value here
 * and it is implementation dependent whether std::thread::hardware_concurrency()
 * returns only the cpus currently online, or includes offline hot plug cpus.
 *
 * See external/libcxx/src/thread.cpp.
 */
size_t get_number_cpus() {
    static constinit std::atomic<size_t> n{};  // zero initialized.
    size_t value = n.load(std::memory_order_relaxed);
    if (value == 0) {  // not set, so we fetch.
        value = std::thread::hardware_concurrency();
        n.store(value, std::memory_order_relaxed);  // on race, this store is idempotent.
    }
    return value;
}

status_t set_priority_for_binder_callback(const char* calling_func) {
    constexpr int priority = nice_to_unified_priority(ANDROID_PRIORITY_URGENT_AUDIO);
    const status_t status = set_thread_priority(gettid_wrapper(), priority);
    ALOGD_IF(status != OK, "%s: set priority %d failed with status %d",
             calling_func, priority, status);
    return status;
}

status_t set_thread_name(std::thread& thread, const std::string& name) {
    const int err = pthread_setname_np(thread.native_handle(), name.substr(0, 15).c_str());
    return err == 0 ? OK : -err;
}

status_t set_thread_name(const std::string& name) {
    const int err = pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
    return err == 0 ? OK : -err;
}

std::string get_thread_name(std::thread& thread) {
    char buf[16]; // Linux limit for pthread name is 16 including null terminator
    if (pthread_getname_np(thread.native_handle(), buf, sizeof(buf)) == 0) {
        return buf;
    }
    return "";
}

std::string get_thread_name(pid_t tid) {
    if (tid == 0 || tid == gettid_wrapper()) {
        char buf[16]; // Linux limit for pthread name is 16 including null terminator
        if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0) {
            return buf;
        }
    } else {
        char path[32];
        snprintf(path, sizeof(path), "/proc/%d/comm", tid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char buf[16];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                if (buf[n - 1] == '\n') n--;
                buf[n] = '\0';
                return buf;
            }
        }
    }
    return "";
}

} // namespace android::audio_utils
