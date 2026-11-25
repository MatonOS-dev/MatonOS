/*
 * Copyright (C) 2026 MatonOS
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_runtime_combinations.h"

#include <android-base/logging.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <netlink/attr.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <sys/socket.h>

#include <array>
#include <set>
#include <utility>

namespace {
using aidl::android::hardware::wifi::IfaceConcurrencyType;
using aidl::android::hardware::wifi::IWifiChip;

struct QueryResult {
    std::vector<IWifiChip::ChipConcurrencyCombination> combinations;
    bool failed = false;
    bool done = false;
};

void addType(std::set<IfaceConcurrencyType>* types, int type) {
    switch (type) {
        case NL80211_IFTYPE_STATION:
            types->insert(IfaceConcurrencyType::STA);
            break;
        case NL80211_IFTYPE_AP:
            types->insert(IfaceConcurrencyType::AP);
            break;
        case NL80211_IFTYPE_P2P_CLIENT:
        case NL80211_IFTYPE_P2P_DEVICE:
        case NL80211_IFTYPE_P2P_GO:
            types->insert(IfaceConcurrencyType::P2P);
            break;
        case NL80211_IFTYPE_NAN:
            types->insert(IfaceConcurrencyType::NAN_IFACE);
            break;
        default:
            break;
    }
}

int parseValidMessage(struct nl_msg* message, void* opaque) {
    auto* result = static_cast<QueryResult*>(opaque);
    auto* header = nlmsg_hdr(message);
    auto* generic = static_cast<genlmsghdr*>(nlmsg_data(header));
    std::array<nlattr*, NL80211_ATTR_MAX + 1> attrs{};
    if (nla_parse(attrs.data(), NL80211_ATTR_MAX, genlmsg_attrdata(generic, 0),
                  genlmsg_attrlen(generic, 0), nullptr) < 0) {
        LOG(ERROR) << "nl80211 GET_WIPHY returned malformed attributes";
        result->failed = true;
        return NL_STOP;
    }
    if (attrs[NL80211_ATTR_INTERFACE_COMBINATIONS] != nullptr) {
        int combinations_remaining = 0;
        nlattr* combination_attr = nullptr;
        nla_for_each_nested(combination_attr, attrs[NL80211_ATTR_INTERFACE_COMBINATIONS],
                            combinations_remaining) {
            std::array<nlattr*, MAX_NL80211_IFACE_COMB + 1> combination_attrs{};
            if (nla_parse_nested(combination_attrs.data(), MAX_NL80211_IFACE_COMB,
                                 combination_attr, nullptr) < 0 ||
                combination_attrs[NL80211_IFACE_COMB_LIMITS] == nullptr) {
                LOG(ERROR) << "nl80211 GET_WIPHY returned malformed interface combination";
                result->failed = true;
                return NL_STOP;
            }

            IWifiChip::ChipConcurrencyCombination combination;
            int limits_remaining = 0;
            nlattr* limit_attr = nullptr;
            nla_for_each_nested(limit_attr, combination_attrs[NL80211_IFACE_COMB_LIMITS],
                                limits_remaining) {
                std::array<nlattr*, MAX_NL80211_IFACE_LIMIT + 1> limit_attrs{};
                if (nla_parse_nested(limit_attrs.data(), MAX_NL80211_IFACE_LIMIT, limit_attr,
                                     nullptr) < 0 ||
                    limit_attrs[NL80211_IFACE_LIMIT_MAX] == nullptr ||
                    limit_attrs[NL80211_IFACE_LIMIT_TYPES] == nullptr) {
                    LOG(ERROR) << "nl80211 GET_WIPHY returned malformed interface limit";
                    result->failed = true;
                    return NL_STOP;
                }

                std::set<IfaceConcurrencyType> types;
                int types_remaining = 0;
                nlattr* type_attr = nullptr;
                nla_for_each_nested(type_attr, limit_attrs[NL80211_IFACE_LIMIT_TYPES],
                                    types_remaining) {
                    addType(&types, nla_type(type_attr));
                }
                if (types.empty()) continue;

                IWifiChip::ChipConcurrencyCombinationLimit limit;
                limit.maxIfaces = nla_get_u32(limit_attrs[NL80211_IFACE_LIMIT_MAX]);
                limit.types.assign(types.begin(), types.end());
                combination.limits.push_back(std::move(limit));
            }
            if (!combination.limits.empty()) {
                result->combinations.push_back(std::move(combination));
            }
        }
        if (combinations_remaining < 0) {
            result->failed = true;
            return NL_STOP;
        }
    }

    // nl80211 omits combinations of one interface: each individually
    // supported type is implicitly available by itself.
    if (attrs[NL80211_ATTR_SUPPORTED_IFTYPES] != nullptr) {
        int types_remaining = 0;
        nlattr* type_attr = nullptr;
        nla_for_each_nested(type_attr, attrs[NL80211_ATTR_SUPPORTED_IFTYPES], types_remaining) {
            std::set<IfaceConcurrencyType> types;
            addType(&types, nla_type(type_attr));
            if (types.empty()) continue;
            IWifiChip::ChipConcurrencyCombinationLimit limit;
            limit.maxIfaces = 1;
            limit.types.assign(types.begin(), types.end());
            IWifiChip::ChipConcurrencyCombination single_iface;
            single_iface.limits.push_back(std::move(limit));
            result->combinations.push_back(std::move(single_iface));
        }
        if (types_remaining < 0) {
            result->failed = true;
            return NL_STOP;
        }
    }
    result->done = true;
    return NL_STOP;
}

bool queryCombinations(unsigned ifindex,
                       std::vector<IWifiChip::ChipConcurrencyCombination>* combinations) {
    struct nl_sock* socket = nl_socket_alloc();
    if (socket == nullptr) {
        PLOG(ERROR) << "Wi-Fi HAL nl_socket_alloc failed";
        return false;
    }

    bool success = false;
    struct nl_cb* callbacks = nullptr;
    do {
        int nl_status = genl_connect(socket);
        if (nl_status < 0) {
            LOG(ERROR) << "Wi-Fi HAL genl_connect failed: " << nl_geterror(nl_status);
            break;
        }
        // The fd exists only after genl_connect().
        timeval timeout{1, 0};
        if (setsockopt(nl_socket_get_fd(socket), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout)) < 0) {
            PLOG(ERROR) << "Wi-Fi HAL failed to bound nl80211 receive time";
            break;
        }

        const int family = genl_ctrl_resolve(socket, "nl80211");
        if (family < 0) {
            LOG(ERROR) << "Wi-Fi HAL could not resolve nl80211 family: "
                       << nl_geterror(family);
            break;
        }
        struct nl_msg* message = nlmsg_alloc();
        if (message == nullptr) {
            PLOG(ERROR) << "Wi-Fi HAL nlmsg_alloc failed";
            break;
        }
        if (genlmsg_put(message, NL_AUTO_PORT, NL_AUTO_SEQ, family, 0, 0,
                        NL80211_CMD_GET_WIPHY, 0) == nullptr ||
            nla_put_u32(message, NL80211_ATTR_IFINDEX, ifindex) < 0) {
            LOG(ERROR) << "Wi-Fi HAL could not encode nl80211 GET_WIPHY for ifindex="
                       << ifindex;
            nlmsg_free(message);
            break;
        }

        QueryResult result;
        callbacks = nl_cb_alloc(NL_CB_DEFAULT);
        if (callbacks == nullptr ||
            nl_cb_set(callbacks, NL_CB_VALID, NL_CB_CUSTOM, parseValidMessage, &result) < 0) {
            PLOG(ERROR) << "Wi-Fi HAL could not configure nl80211 callbacks";
            nlmsg_free(message);
            break;
        }
        nl_status = nl_send_auto(socket, message);
        if (nl_status < 0) {
            LOG(ERROR) << "Wi-Fi HAL nl80211 GET_WIPHY send failed: "
                       << nl_geterror(nl_status);
            nlmsg_free(message);
            break;
        }
        nlmsg_free(message);
        const int status = nl_recvmsgs(socket, callbacks);
        if (status < 0) {
            LOG(ERROR) << "Wi-Fi HAL nl80211 GET_WIPHY receive failed: "
                       << nl_geterror(status);
        }
        if (status >= 0 && result.done && !result.failed && !result.combinations.empty()) {
            *combinations = std::move(result.combinations);
            success = true;
        }
    } while (false);

    if (callbacks != nullptr) nl_cb_put(callbacks);
    nl_socket_free(socket);
    return success;
}
}  // namespace

bool getRuntimeIfaceCombinations(
        const std::string& iface_name,
        std::vector<IWifiChip::ChipConcurrencyCombination>* out) {
    if (out == nullptr) return false;
    const unsigned ifindex = if_nametoindex(iface_name.c_str());
    if (ifindex == 0) return false;  // Expected when hardware is absent or being renamed.
    return queryCombinations(ifindex, out);
}
