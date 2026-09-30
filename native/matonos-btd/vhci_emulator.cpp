/* SPDX-License-Identifier: Apache-2.0 */
#include "vhci_emulator.h"
#include "maton_log.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint8_t kCommand = 0x01;
constexpr uint8_t kEvent = 0x04;
constexpr uint8_t kVendor = 0xff;
constexpr size_t kMaxHciPacket = 1 + 4 + 65535;

std::vector<uint8_t> Complete(uint16_t opcode, std::vector<uint8_t> params) {
    if (params.size() > UINT8_MAX - 3) return {};
    std::vector<uint8_t> event{ kEvent, 0x0e,
        static_cast<uint8_t>(3 + params.size()), 1,
        static_cast<uint8_t>(opcode), static_cast<uint8_t>(opcode >> 8) };
    event.insert(event.end(), params.begin(), params.end());
    return event;
}

std::vector<uint8_t> Reply(const uint8_t* packet, size_t size) {
    if (size < 4 || packet[0] != kCommand) return {};
    if (packet[3] != size - 4) return {};
    const uint16_t opcode = packet[1] | (static_cast<uint16_t>(packet[2]) << 8);
    const uint16_t ogf = opcode >> 10;
    const uint16_t ocf = opcode & 0x03ff;
    std::vector<uint8_t> result{0x00};  // HCI success status
    if (opcode == 0x1001) { // Read Local Version Information
        result.insert(result.end(), {0x0b, 0x0b, 0x00, 0x00, 0x0b, 0x00, 0x0b, 0x00});
    } else if (opcode == 0x1002) { // Read Local Supported Commands
        result.resize(65, 0xff);
    } else if (opcode == 0x1003) { // Read Local Supported Features
        result.insert(result.end(), {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
    } else if (opcode == 0x1004) { // Read Local Extended Features
        result.insert(result.end(), {0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
    } else if (opcode == 0x1005) { // Read Buffer Size
        result.insert(result.end(), {0xfb,0x00,0x00,0x10,0x00,0x10,0x00});
    } else if (opcode == 0x1009) { // Read BD_ADDR
        result.insert(result.end(), {0x02,0x00,0x00,0x00,0x00,0x01});
    } else if (ogf == 0x08 && ocf == 0x0002) { // LE Read Buffer Size
        result.insert(result.end(), {0xfb,0x00,0x10});
    } else if (opcode == 0x2060) { // LE Read Buffer Size v2
        result.insert(result.end(), {0xfb,0x00,0x10,0xfb,0x00,0x10});
    } else if (ogf == 0x08 && ocf == 0x0003) { // LE Read Local Supported Features
        result.insert(result.end(), {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
    } else if (ogf == 0x08 && ocf == 0x000f) { // LE Read Filter Accept List Size
        result.push_back(0x00);
    } else if (ogf == 0x08 && ocf == 0x002a) { // LE Read Resolving List Size
        result.push_back(0x00);
    } else if (ogf == 0x08 && ocf == 0x002f) { // LE Read Maximum Data Length
        result.insert(result.end(), {0xfb,0x00,0x48,0x08,0xfb,0x00,0x48,0x08});
    } else if (ogf == 0x08 && ocf == 0x003a) { // LE Read Maximum Advertising Data Length
        result.insert(result.end(), {0xfb,0x00});
    } else if (ogf == 0x08 && ocf == 0x003b) { // LE Read Number of Supported Advertising Sets
        result.push_back(0x00);
    } else if (ogf == 0x08 && ocf == 0x001c) { // LE Read Supported States
        result.insert(result.end(), {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
    } else {
        return {kEvent, 0x0f, 0x04, 0x01, 0x00,
                static_cast<uint8_t>(opcode), static_cast<uint8_t>(opcode >> 8)};
    }
    return Complete(opcode, std::move(result));
}
} // namespace

int RunVhciEmulator() {
    const int fd = open("/dev/vhci", O_RDWR | O_CLOEXEC);
    if (fd < 0) { ALOGE("open /dev/vhci failed: %s", strerror(errno)); return 1; }
    // Bit 7 requests a raw controller. Without it Linux starts its own HCI
    // setup, leaving the device busy when our HAL claims the user channel.
    const uint8_t create[] = {kVendor, 0x80};
    size_t written = 0;
    while (written < sizeof(create)) {
            ssize_t count = write(fd, create + written, sizeof(create) - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ALOGE("creating virtual HCI failed: %s", strerror(errno)); close(fd); return 1; }
            written += static_cast<size_t>(count);
    }
    ALOGI("VHCI controller created; serving an empty virtual radio");
    uint8_t packet[kMaxHciPacket];
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        int ready;
        do { ready = poll(&pfd, 1, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        ssize_t count = read(fd, packet, sizeof(packet));
        if (count <= 0) { if (errno == EINTR) continue; break; }
        if (count >= 4 && packet[0] == kVendor && packet[1] == 0x80) {
            char index[16];
            snprintf(index, sizeof(index), "%u", static_cast<unsigned>(packet[2] | (packet[3] << 8)));
            __system_property_set("vendor.maton.bluetooth.vhci_index", index);
            ALOGI("VHCI assigned HCI index %s", index);
            continue;
        }
        auto response = Reply(packet, static_cast<size_t>(count));
        size_t offset = 0;
        while (offset < response.size()) {
            ssize_t count = write(fd, response.data() + offset, response.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ALOGE("writing VHCI response failed: %s", strerror(errno)); goto done; }
            offset += static_cast<size_t>(count);
        }
    }
done:
    close(fd);
    return 0;
}
