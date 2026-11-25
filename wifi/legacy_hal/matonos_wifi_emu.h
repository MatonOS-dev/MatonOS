// Copyright (C) 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hardware_legacy/wifi_hal.h>

namespace matonos_wifi_emu {
// The forked nl80211 backend's function table (libwifi-hal-matonos).
wifi_error init_wifi_vendor_hal_func_table(wifi_hal_fn* fn);
}  // namespace matonos_wifi_emu
