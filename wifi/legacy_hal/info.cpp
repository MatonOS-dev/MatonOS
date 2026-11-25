/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "info.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <net/if.h>
#include <sys/stat.h>

#include <string>

#include "log.h"

namespace matonos_wifi_emu {

// MatonOS: upstream goldfish only knew "wlan0"/"wlan1" as they existed at
// start. Interfaces are created later (nl80211 AP/STA create in the Wi-Fi
// HAL) and renamed (wifid card switch), so list every wireless netdev each
// time the HAL asks.
Info::Info() {
}

bool Info::init() {
    if (!mNetlink.init()) {
        return false;
    }
    refreshInterfaces();
    return true;
}

void Info::refreshInterfaces() {
    mInterfaceHandles.clear();
    DIR* dir = opendir("/sys/class/net");
    if (dir == nullptr) {
        ALOGE("opendir /sys/class/net failed: %s", strerror(errno));
        return;
    }
    while (struct dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        struct stat ignored_statbuf;
        const std::string phy = "/sys/class/net/" + name + "/phy80211";
        if (stat(phy.c_str(), &ignored_statbuf) != 0) continue;
        const unsigned int index = if_nametoindex(name.c_str());
        if (index == 0) continue;  // renamed or removed since readdir
        Interface* found = nullptr;
        for (auto& iface : mInterfaces) {
            if (iface->name() == name && iface->index() == index) {
                found = iface.get();
                break;
            }
        }
        if (found == nullptr) {
            auto iface = std::make_unique<Interface>(mNetlink, name.c_str());
            if (!iface->init()) continue;
            found = iface.get();
            mInterfaces.push_back(std::move(iface));
        }
        mInterfaceHandles.push_back(reinterpret_cast<wifi_interface_handle>(found));
    }
    closedir(dir);
}

void Info::eventLoop() {
    mNetlink.eventLoop();
}

void Info::stop(StopHandler stopHandler) {
    mNetlink.stop(stopHandler);
}

wifi_error Info::getInterfaces(int* num, wifi_interface_handle** interfaces) {
    if (num == nullptr || interfaces == nullptr) {
        return WIFI_ERROR_INVALID_ARGS;
    }
    refreshInterfaces();
    *num = mInterfaceHandles.size();
    *interfaces = mInterfaceHandles.data();

    return WIFI_SUCCESS;
}

}  // namespace matonos_wifi_emu
