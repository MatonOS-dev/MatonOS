#include "matonos_ipc.h"

#include <algorithm>
#include <android/log.h>

#include <android/binder_auto_utils.h>
#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define LOG_TAG "MatonosIpc"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {
constexpr size_t kMaxMessage = 64 * 1024;
constexpr int kMaxDepth = 32;
constexpr int kMaxNodes = 8192;

struct JsonValue {
    enum class Type { kObject, kArray, kString, kNumber, kBool, kNull } type;
    std::string scalar;
    std::string raw;
    std::unordered_map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
};

class JsonParser {
  public:
    explicit JsonParser(const std::string &s) : s_(s) {}
    bool Parse(JsonValue *out) {
        Skip();
        if (!Value(out, 0)) return false;
        Skip();
        return pos_ == s_.size();
    }
  private:
    bool Value(JsonValue *out, int depth) {
        if (depth > kMaxDepth || ++nodes_ > kMaxNodes) return false;
        Skip();
        size_t start = pos_;
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') {
            out->type = JsonValue::Type::kObject;
            ++pos_; Skip();
            if (Take('}')) { out->raw = s_.substr(start, pos_ - start); return true; }
            while (true) {
                std::string key;
                if (!String(&key)) return false;
                Skip(); if (!Take(':')) return false;
                JsonValue child;
                if (!Value(&child, depth + 1) || !out->object.emplace(key, std::move(child)).second) return false;
                Skip();
                if (Take('}')) break;
                if (!Take(',')) return false;
                Skip();
            }
        } else if (c == '[') {
            out->type = JsonValue::Type::kArray;
            ++pos_; Skip();
            if (!Take(']')) {
                while (true) {
                    JsonValue child;
                    if (!Value(&child, depth + 1)) return false;
                    out->array.push_back(std::move(child));
                    Skip();
                    if (Take(']')) break;
                    if (!Take(',')) return false;
                    Skip();
                }
            }
        } else if (c == '"') {
            out->type = JsonValue::Type::kString;
            if (!String(&out->scalar)) return false;
        } else if (c == 't' || c == 'f') {
            out->type = JsonValue::Type::kBool;
            const char *word = c == 't' ? "true" : "false";
            size_t len = std::strlen(word);
            if (s_.compare(pos_, len, word) != 0) return false;
            out->scalar.assign(word, len); pos_ += len;
        } else if (c == 'n') {
            out->type = JsonValue::Type::kNull;
            if (s_.compare(pos_, 4, "null") != 0) return false;
            out->scalar = "null"; pos_ += 4;
        } else {
            out->type = JsonValue::Type::kNumber;
            if (!Number(&out->scalar)) return false;
        }
        out->raw = s_.substr(start, pos_ - start);
        return true;
    }
    bool String(std::string *decoded) {
        if (!Take('"')) return false;
        decoded->clear();
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { decoded->push_back(static_cast<char>(c)); continue; }
            if (pos_ >= s_.size()) return false;
            char e = s_[pos_++];
            switch (e) {
                case '"': case '\\': case '/': decoded->push_back(e); break;
                case 'b': decoded->push_back('\b'); break;
                case 'f': decoded->push_back('\f'); break;
                case 'n': decoded->push_back('\n'); break;
                case 'r': decoded->push_back('\r'); break;
                case 't': decoded->push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return false;
                    unsigned value = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s_[pos_++];
                        value <<= 4;
                        if (h >= '0' && h <= '9') value |= h - '0';
                        else if (h >= 'a' && h <= 'f') value |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') value |= h - 'A' + 10;
                        else return false;
                    }
                    if (value >= 0xd800 && value <= 0xdbff) {
                        if (pos_ + 6 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return false;
                        pos_ += 2;
                        unsigned low = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            low <<= 4;
                            if (h >= '0' && h <= '9') low |= h - '0';
                            else if (h >= 'a' && h <= 'f') low |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') low |= h - 'A' + 10;
                            else return false;
                        }
                        if (low < 0xdc00 || low > 0xdfff) return false;
                        value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
                    } else if (value >= 0xdc00 && value <= 0xdfff) {
                        return false;
                    }
                    if (value <= 0x7f) decoded->push_back(static_cast<char>(value));
                    else if (value <= 0x7ff) {
                        decoded->push_back(static_cast<char>(0xc0 | (value >> 6)));
                        decoded->push_back(static_cast<char>(0x80 | (value & 0x3f)));
                    } else if (value <= 0xffff) {
                        decoded->push_back(static_cast<char>(0xe0 | (value >> 12)));
                        decoded->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                        decoded->push_back(static_cast<char>(0x80 | (value & 0x3f)));
                    } else {
                        decoded->push_back(static_cast<char>(0xf0 | (value >> 18)));
                        decoded->push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
                        decoded->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                        decoded->push_back(static_cast<char>(0x80 | (value & 0x3f)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Number(std::string *value) {
        size_t start = pos_;
        Take('-');
        if (Take('0')) {
            if (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') return false;
        } else {
            if (pos_ >= s_.size() || s_[pos_] < '1' || s_[pos_] > '9') return false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (Take('.')) {
            size_t fraction = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (fraction == pos_) return false;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_; if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            size_t exponent = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (exponent == pos_) return false;
        }
        if (start == pos_) return false;
        *value = s_.substr(start, pos_ - start);
        return true;
    }
    void Skip() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) ++pos_; }
    bool Take(char c) { if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; } return false; }
    const std::string &s_;
    size_t pos_ = 0;
    int nodes_ = 0;
};

bool ValidUtf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i++]);
        if (c <= 0x7f) continue;
        int extra;
        unsigned char second_min = 0x80, second_max = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) extra = 1;
        else if (c >= 0xe0 && c <= 0xef) {
            extra = 2;
            if (c == 0xe0) second_min = 0xa0;
            if (c == 0xed) second_max = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            extra = 3;
            if (c == 0xf0) second_min = 0x90;
            if (c == 0xf4) second_max = 0x8f;
        } else return false;
        if (i + static_cast<size_t>(extra) > s.size()) return false;
        unsigned char second = static_cast<unsigned char>(s[i++]);
        if (second < second_min || second > second_max) return false;
        for (int j = 1; j < extra; ++j) {
            unsigned char continuation = static_cast<unsigned char>(s[i++]);
            if (continuation < 0x80 || continuation > 0xbf) return false;
        }
    }
    return true;
}

bool ValidJson(const std::string &json, JsonValue *value) {
    return !json.empty() && json.size() <= kMaxMessage && ValidUtf8(json) && JsonParser(json).Parse(value);
}

bool ValidName(const std::string &name) {
    if (name.empty() || name.size() > 32 || name[0] < 'a' || name[0] > 'z') return false;
    for (char c : name) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
}  // namespace

#include <aidl/vendor/matonos/channel/BnChannel.h>
#include <aidl/vendor/matonos/channel/IChannelListener.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/binder_status.h>

using aidl::vendor::matonos::channel::BnChannel;
using aidl::vendor::matonos::channel::IChannelListener;
using ndk::ScopedAStatus;

struct MatonosIpcServer {
    using Handler = std::pair<matonos_ipc_handler, void *>;
    explicit MatonosIpcServer(std::string target) : target(std::move(target)) {}
    std::string target;
    std::mutex handlers_mutex;
    std::unordered_map<std::string, Handler> handlers;
    std::mutex listeners_mutex;
    std::unordered_map<std::string, std::vector<std::shared_ptr<IChannelListener>>> listeners;
    std::shared_ptr<void> service_lifetime;
};

namespace {
class Channel final : public BnChannel {
  public:
    explicit Channel(MatonosIpcServer *server) : server_(server) {}
    ScopedAStatus call(const std::string &command, const std::string &jsonArgs,
                       std::string *jsonResult) override {
        if (command.empty() || command.size() > 64 || jsonArgs.size() > kMaxMessage)
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        JsonValue args;
        if (!ValidJson(jsonArgs, &args) || args.type != JsonValue::Type::kObject)
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        MatonosIpcServer::Handler handler;
        {
            std::lock_guard<std::mutex> lock(server_->handlers_mutex);
            auto it = server_->handlers.find(command);
            if (it == server_->handlers.end()) return ScopedAStatus::fromServiceSpecificError(1);
            handler = it->second;
        }
        std::vector<char> result(kMaxMessage + 1, 0);
        if (handler.first(jsonArgs.c_str(), result.data(), result.size(), handler.second) != 0)
            return ScopedAStatus::fromServiceSpecificError(2);
        const size_t result_size = strnlen(result.data(), result.size());
        if (result_size > kMaxMessage) return ScopedAStatus::fromServiceSpecificError(3);
        std::string value(result.data(), result_size);
        JsonValue parsed;
        if (!ValidJson(value, &parsed)) return ScopedAStatus::fromServiceSpecificError(3);
        *jsonResult = std::move(value);
        return ScopedAStatus::ok();
    }
    ScopedAStatus subscribe(const std::string &topic,
                            const std::shared_ptr<IChannelListener> &listener) override {
        if (!ValidName(topic) || !listener) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::lock_guard<std::mutex> lock(server_->listeners_mutex);
        auto &items = server_->listeners[topic];
        for (const auto &item : items) if (item->asBinder() == listener->asBinder()) return ScopedAStatus::ok();
        items.push_back(listener);
        return ScopedAStatus::ok();
    }
    ScopedAStatus unsubscribe(const std::string &topic,
                              const std::shared_ptr<IChannelListener> &listener) override {
        if (!listener) return ScopedAStatus::ok();
        std::lock_guard<std::mutex> lock(server_->listeners_mutex);
        auto it = server_->listeners.find(topic);
        if (it != server_->listeners.end()) {
            auto &items = it->second;
            items.erase(std::remove_if(items.begin(), items.end(), [&](const auto &item) {
                return item->asBinder() == listener->asBinder();
            }), items.end());
            if (items.empty()) server_->listeners.erase(it);
        }
        return ScopedAStatus::ok();
    }
  private:
    MatonosIpcServer *server_;
};
}  // namespace

extern "C" MatonosIpcServer *matonos_ipc_create(const char *target) {
    if (!target || !ValidName(target)) return nullptr;
    return new MatonosIpcServer(target);
}
extern "C" int matonos_ipc_register(MatonosIpcServer *server, const char *command,
                                     matonos_ipc_handler handler, void *context) {
    if (!server || !command || !handler || !*command || std::strlen(command) > 64) return -1;
    std::lock_guard<std::mutex> lock(server->handlers_mutex);
    return server->handlers.emplace(command, std::make_pair(handler, context)).second ? 0 : -1;
}
extern "C" int matonos_ipc_start(MatonosIpcServer *server) {
    if (!server) return -1;
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto service = ndk::SharedRefBase::make<Channel>(server);
    server->service_lifetime = service;
    std::string instance = std::string(BnChannel::descriptor) + "/" + server->target;
    if (AServiceManager_addService(service->asBinder().get(), instance.c_str()) != STATUS_OK) return -1;
    ABinderProcess_startThreadPool();
    return 0;
}
extern "C" int matonos_ipc_publish(MatonosIpcServer *server, const char *topic,
                                    const char *json_event) {
    if (!server || !topic || !json_event || !ValidName(topic)) return -1;
    std::string event(json_event);
    JsonValue parsed;
    if (!ValidJson(event, &parsed)) return -1;
    std::vector<std::shared_ptr<IChannelListener>> listeners;
    {
        std::lock_guard<std::mutex> lock(server->listeners_mutex);
        auto it = server->listeners.find(topic);
        if (it == server->listeners.end()) return 0;
        listeners = it->second;
    }
    int delivered = 0;
    std::vector<std::shared_ptr<IChannelListener>> dead;
    for (const auto &listener : listeners) {
        auto status = listener->onEvent(topic, event);
        if (status.isOk()) ++delivered;
        else dead.push_back(listener);
    }
    if (!dead.empty()) {
        std::lock_guard<std::mutex> lock(server->listeners_mutex);
        auto it = server->listeners.find(topic);
        if (it != server->listeners.end()) {
            auto &items = it->second;
            items.erase(std::remove_if(items.begin(), items.end(), [&](const auto &item) {
                return std::find(dead.begin(), dead.end(), item) != dead.end();
            }), items.end());
            if (items.empty()) server->listeners.erase(it);
        }
    }
    return delivered;
}
