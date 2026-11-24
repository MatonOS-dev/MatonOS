/*
 * Copyright 2022 The Android Open Source Project
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

#define LOG_TAG "android.hardware.bluetooth.service.matonos"

#include "BluetoothHci.h"

#include <cutils/properties.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <string.h>
#include <system_error>
#include <sys/uio.h>
#include <termios.h>

#include "log/log.h"

namespace {
int SetTerminalRaw(int fd) {
    termios terminal_settings;
    int rval = tcgetattr(fd, &terminal_settings);
    if (rval < 0) {
        return rval;
    }
    cfmakeraw(&terminal_settings);
    rval = tcsetattr(fd, TCSANOW, &terminal_settings);
    return rval;
}
}  // namespace

using namespace ::android::hardware::bluetooth::hci;
using namespace ::android::hardware::bluetooth::async;
using aidl::android::hardware::bluetooth::hal::Status;

namespace aidl::android::hardware::bluetooth::impl {

std::optional<std::string> GetSystemProperty(const std::string& property) {
    std::array<char, PROPERTY_VALUE_MAX> value_array{0};
    auto value_len = property_get(property.c_str(), value_array.data(), nullptr);
    if (value_len <= 0) {
        return std::nullopt;
    }
    return std::string(value_array.data(), value_len);
}

bool starts_with(const std::string& str, const std::string& prefix) {
    return str.compare(0, prefix.length(), prefix) == 0;
}

BluetoothHci::BluetoothHci(const std::string& dev_path) {
    char property_bytes[PROPERTY_VALUE_MAX];
    property_get("vendor.ser.bt-uart", property_bytes, dev_path.c_str());
    mDevPath = std::string(property_bytes);
}

int BluetoothHci::getFdFromDevPath() {
    int fd = open(mDevPath.c_str(), O_RDWR);
    if (fd < 0) {
        ALOGE("Could not connect to bt: %s (%s)", mDevPath.c_str(), strerror(errno));
        return fd;
    }
    if (int ret = SetTerminalRaw(fd) < 0) {
        ALOGI("Could not make %s a raw terminal %d(%s)", mDevPath.c_str(), ret, strerror(errno));
    }
    return fd;
}

void BluetoothHci::reset(int fd) {
    // Send a reset command and wait until the command complete comes back.

    std::vector<uint8_t> reset = {0x03, 0x0c, 0x00};

    auto resetPromise = std::make_shared<std::promise<void>>();
    auto resetFuture = resetPromise->get_future();

    auto reset_h4 = std::make_shared<H4Protocol>(
            fd,
            [](const std::vector<uint8_t>& raw_command) {
                ALOGI("Discarding %d bytes with command type",
                      static_cast<int>(raw_command.size()));
            },
            [](const std::vector<uint8_t>& raw_acl) {
                ALOGI("Discarding %d bytes with acl type", static_cast<int>(raw_acl.size()));
            },
            [](const std::vector<uint8_t>& raw_sco) {
                ALOGI("Discarding %d bytes with sco type", static_cast<int>(raw_sco.size()));
            },
            [resetPromise](const std::vector<uint8_t>& raw_event) {
                std::vector<uint8_t> reset_complete = {0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00};
                bool valid = raw_event.size() == 6 && raw_event[0] == reset_complete[0] &&
                             raw_event[1] == reset_complete[1] &&
                             // Don't compare the number of packets field.
                             raw_event[3] == reset_complete[3] &&
                             raw_event[4] == reset_complete[4] && raw_event[5] == reset_complete[5];
                if (valid) {
                    resetPromise->set_value();
                } else {
                    ALOGI("Discarding %d bytes with event type",
                          static_cast<int>(raw_event.size()));
                }
            },
            [](const std::vector<uint8_t>& raw_iso) {
                ALOGI("Discarding %d bytes with iso type", static_cast<int>(raw_iso.size()));
            },
            [this]() {
                ALOGI("HCI socket device disconnected while waiting for reset");
                mFdWatcher.StopWatchingFileDescriptors();
            });
    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        if (mState != HalState::INITIALIZING) return;
        mFdWatcher.WatchFdForNonBlockingReads(fd, [reset_h4](int) { reset_h4->OnDataReady(); });
    }

    reset_h4->Send(PacketType::COMMAND, reset);
    auto status = resetFuture.wait_for(std::chrono::seconds(1));
    mFdWatcher.StopWatchingFileDescriptors();
    if (status == std::future_status::ready) {
        ALOGI("HCI Reset successful");
    } else {
        ALOGE("HCI Reset Response not received in one second");
    }

    resetPromise.reset();
}

#define CLOSE_FD(fd) do { \
    if ((fd) >= 0) { \
        ALOGI("close fd=%d at %s:%d", (fd), __FILE__, __LINE__); \
        if (::close(fd) < 0) ALOGE("close fd=%d failed at %s:%d: %s", (fd), __FILE__, __LINE__, strerror(errno)); \
    } \
} while (0)

void BluetoothHci::initialize(const std::shared_ptr<hal::IBluetoothHciCallbacks>& cb) {
    ALOGI(__func__);
    if (cb == nullptr) {
        ALOGE("cb == nullptr! -> Unable to call initializationComplete(ERR)");
        abort();
    }

    std::unique_lock<std::mutex> lifecycle_guard(mLifecycleMutex);

    // The stack is the only client. A new initialize() while a previous
    // setup is still waiting for a controller, or after the previous client
    // died, means that session is gone: cancel and clean it up first. This
    // never waits for hardware (the wait is cancellable).
    HalState old_state;
    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        old_state = mState;
    }
    if (old_state == HalState::INITIALIZING ||
        (old_state == HalState::ONE_CLIENT && mClientDead)) {
        ALOGI("initialize: replacing stale session (state %d)", static_cast<int>(old_state));
        closeLockedLifecycle();
    }
    // A worker that already finished (success or failure) may still be joinable.
    if (mInitializationThread.joinable()) mInitializationThread.join();

    std::shared_ptr<NetBluetoothMgmt> management;
    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        old_state = mState;
        if (mState == HalState::READY) {
            mState = HalState::INITIALIZING;
            mCb = cb;
            mClientDead = false;
            management_ = std::make_shared<NetBluetoothMgmt>();
            management = management_;
            management->resetCancellation();
        }
    }
    if (old_state != HalState::READY) {
        ALOGE("initialize: Unexpected State %d", static_cast<int>(old_state));
        lifecycle_guard.unlock();
        cb->initializationComplete(Status::ALREADY_INITIALIZED);
        return;
    }

    try {
        mInitializationThread = std::thread(&BluetoothHci::initializeWorker, this, cb, management);
    } catch (const std::system_error& error) {
        ALOGE("starting Bluetooth initialization worker failed: %s", error.what());
        {
            std::lock_guard<std::mutex> guard(mStateMutex);
            if (mState == HalState::INITIALIZING) {
                mState = HalState::READY;
                mCb.reset();
                management_.reset();
            }
        }
        lifecycle_guard.unlock();
        cb->initializationComplete(Status::HARDWARE_INITIALIZATION_ERROR);
    }
}

void BluetoothHci::initializeWorker(
        const std::shared_ptr<hal::IBluetoothHciCallbacks>& cb,
        const std::shared_ptr<NetBluetoothMgmt>& management) {
    int fd = management->openHci();
    bool management_fd = fd >= 0;
    if (fd < 0) {
        bool cancelled;
        {
            std::lock_guard<std::mutex> guard(mStateMutex);
            cancelled = mState != HalState::INITIALIZING;
            if (!cancelled) {
                // Preserve the stock HAL's serial-device fallback when MGMT
                // discovery fails for a reason other than cancellation.
                management_.reset();
            }
        }
        if (cancelled) return;

        ALOGI("Unable to open Linux interface, trying default path.");
        fd = getFdFromDevPath();
        management_fd = false;
        if (fd < 0) {
            bool notify = false;
            {
                std::lock_guard<std::mutex> guard(mStateMutex);
                if (mState == HalState::INITIALIZING) {
                    mState = HalState::READY;
                    mCb.reset();
                    management_.reset();
                    notify = true;
                }
            }
            if (notify) cb->initializationComplete(Status::UNABLE_TO_OPEN_INTERFACE);
            return;
        }
    }

    // The Cuttlefish/goldfish reset path remains on the setup worker so it
    // cannot block the Binder thread pool.
    const std::string kBoardProperty = "ro.product.board";
    auto board_name = GetSystemProperty(kBoardProperty);
    if (board_name.has_value() &&
        (starts_with(board_name.value(), "cutf") || starts_with(board_name.value(), "goldfish"))) {
        reset(fd);
    }

    auto h4 = std::make_shared<H4Protocol>(
            fd,
            [](const std::vector<uint8_t>& /* raw_command */) {
                LOG_ALWAYS_FATAL("Unexpected command!");
            },
            [this](const std::vector<uint8_t>& raw_acl) {
                std::shared_ptr<hal::IBluetoothHciCallbacks> callback;
                { std::lock_guard<std::mutex> guard(mStateMutex); if (mState == HalState::ONE_CLIENT) callback = mCb; }
                if (callback) callback->aclDataReceived(raw_acl);
            },
            [this](const std::vector<uint8_t>& raw_sco) {
                std::shared_ptr<hal::IBluetoothHciCallbacks> callback;
                { std::lock_guard<std::mutex> guard(mStateMutex); if (mState == HalState::ONE_CLIENT) callback = mCb; }
                if (callback) callback->scoDataReceived(raw_sco);
            },
            [this](const std::vector<uint8_t>& raw_event) {
                std::shared_ptr<hal::IBluetoothHciCallbacks> callback;
                { std::lock_guard<std::mutex> guard(mStateMutex); if (mState == HalState::ONE_CLIENT) callback = mCb; }
                if (callback) callback->hciEventReceived(raw_event);
            },
            [this](const std::vector<uint8_t>& raw_iso) {
                std::shared_ptr<hal::IBluetoothHciCallbacks> callback;
                { std::lock_guard<std::mutex> guard(mStateMutex); if (mState == HalState::ONE_CLIENT) callback = mCb; }
                if (callback) callback->isoDataReceived(raw_iso);
            },
            [this]() {
                ALOGI("HCI socket device disconnected");
                mFdWatcher.StopWatchingFileDescriptors();
            });

    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        if (mState != HalState::INITIALIZING) {
            if (management_fd) management->closeHci(); else CLOSE_FD(fd);
            return;
        }
        mFd = fd;
        mH4 = h4;
        mFdWatcher.WatchFdForNonBlockingReads(fd, [h4](int) { h4->OnDataReady(); });
        mState = HalState::ONE_CLIENT;
    }

    ALOGI("initialization complete");
    cb->initializationComplete(Status::SUCCESS);
}

void BluetoothHci::close() {
    std::lock_guard<std::mutex> lifecycle_guard(mLifecycleMutex);
    closeLockedLifecycle();
}

void BluetoothHci::closeLockedLifecycle() {
    ALOGI(__func__);
    std::shared_ptr<NetBluetoothMgmt> management;
    bool already_closed;
    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        already_closed = mState == HalState::READY;
        if (!already_closed) {
            mState = HalState::CLOSING;
            management = management_;
        }
    }

    if (already_closed) {
        if (mInitializationThread.joinable()) mInitializationThread.join();
        ALOGI("Already closed");
        return;
    }

    if (management) management->cancel();
    mFdWatcher.StopWatchingFileDescriptors();
    if (mInitializationThread.joinable()) mInitializationThread.join();

    int fd;
    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        fd = mFd;
        mFd = -1;
        mCb.reset();
        mH4.reset();
        management_.reset();
        mClientDead = false;
        mState = HalState::READY;
    }
    if (management) {
        management->closeHci();
    } else {
        CLOSE_FD(fd);
    }
}

void BluetoothHci::clientDied() {
    ALOGI(__func__);
    std::lock_guard<std::mutex> lifecycle_guard(mLifecycleMutex);
    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        mClientDead = true;
    }
    closeLockedLifecycle();
}

void BluetoothHci::sendHciCommand(const std::vector<uint8_t>& packet) {
    return send(PacketType::COMMAND, packet);
}

void BluetoothHci::sendAclData(const std::vector<uint8_t>& packet) {
    return send(PacketType::ACL_DATA, packet);
}

void BluetoothHci::sendScoData(const std::vector<uint8_t>& packet) {
    return send(PacketType::SCO_DATA, packet);
}

void BluetoothHci::sendIsoData(const std::vector<uint8_t>& packet) {
    return send(PacketType::ISO_DATA, packet);
}

void BluetoothHci::send(PacketType type, const std::vector<uint8_t>& v) {
    if (v.empty()) {
        ALOGE("Packet is empty, no data was found to be sent");
        abort();
    }

    std::lock_guard<std::mutex> guard(mStateMutex);
    if (mH4 == nullptr) {
        ALOGE("Illegal State");
        abort();
    }

    mH4->Send(type, v);
}

}  // namespace aidl::android::hardware::bluetooth::impl
