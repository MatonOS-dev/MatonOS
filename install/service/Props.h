// NDK property helpers (the installer service is NDK-built: no libbase).
#pragma once
#include <sys/system_properties.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>

namespace matonos::props {

inline bool Set(const char* name, const std::string& value) {
    return __system_property_set(name, value.c_str()) == 0;
}

// Waits until |name| equals |value| or |timeout| passes.
inline bool WaitFor(const char* name, const std::string& value, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const prop_info* pi = nullptr;
    uint32_t serial = 0;
    for (;;) {
        if (!pi) pi = __system_property_find(name);
        if (pi) {
            std::string current;
            __system_property_read_callback(pi, [](void* out, const char*, const char* v, uint32_t) {
                *static_cast<std::string*>(out) = v;
            }, &current);
            if (current == value) return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        timespec ts{static_cast<time_t>(left / 1000000000), static_cast<long>(left % 1000000000)};
        if (pi) {
            __system_property_wait(pi, serial, &serial, &ts);
        } else {
            timespec step{0, 50 * 1000 * 1000};  // property not created yet
            nanosleep(&step, nullptr);
        }
    }
}

}  // namespace matonos::props
