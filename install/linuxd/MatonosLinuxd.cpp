#include <org/matonos/systembridge/BnLinuxd.h>
#include <org/matonos/systembridge/ILinuxdListener.h>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <json/json.h>
#include <utils/String8.h>

#include "FlatpakManager.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using org::matonos::systembridge::BnLinuxd;
using org::matonos::systembridge::ILinuxdListener;

namespace {
constexpr char kInstance[] = "org.matonos.systembridge.ILinuxd/default";
constexpr size_t kInputLimit = 64 * 1024;
std::mutex g_listener_mutex;
std::vector<android::sp<ILinuxdListener>> g_progress_listeners;

bool Parse(const std::string& text, Json::Value* root) {
    if (text.size() > kInputLimit) return false;
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(text.data(), text.data() + text.size(), root, &errors) && root->isObject();
}

std::string Encode(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value Error(const std::string& message) {
    Json::Value result(Json::objectValue);
    result["ok"] = false;
    result["error"] = message;
    return result;
}

bool OnlyKeys(const Json::Value& value, std::initializer_list<const char*> allowed) {
    std::set<std::string> keys;
    for (const char* key : allowed) keys.emplace(key);
    for (const auto& key : value.getMemberNames()) if (!keys.count(key)) return false;
    return true;
}

Json::Value EncodeResult(const FlatpakResult& result) {
    Json::Value value(Json::objectValue);
    value["ok"] = result.ok != 0;
    if (!result.accepted && !result.has_pid) value["exitCode"] = result.exit_code;
    value["output"] = result.output ? result.output : "";
    value["outputTruncated"] = result.output_truncated != 0;
    if (result.accepted) value["accepted"] = true;
    if (result.has_pid) value["pid"] = static_cast<Json::Int64>(result.pid);
    if (result.error) value["error"] = result.error;
    if (result.has_unused_cleanup) {
        Json::Value cleanup(Json::objectValue);
        cleanup["ok"] = result.unused_cleanup_ok != 0;
        cleanup["exitCode"] = result.unused_cleanup_exit_code;
        value["unusedCleanup"] = cleanup;
    }
    return value;
}

void PublishProgress(const std::string& text) {
    Json::Value event(Json::objectValue);
    event["line"] = text.substr(0, 2048);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        size_t begin = i;
        while (begin > 0 && std::isdigit(static_cast<unsigned char>(text[begin - 1]))) --begin;
        if (begin < i) {
            unsigned percent = 0;
            for (size_t j = begin; j < i; ++j)
                percent = std::min(100u, percent * 10u + static_cast<unsigned>(text[j] - '0'));
            event["percent"] = percent;
        }
        break;
    }
    const std::string body = Encode(event);
    std::lock_guard<std::mutex> guard(g_listener_mutex);
    auto listener = g_progress_listeners.begin();
    while (listener != g_progress_listeners.end()) {
        if ((*listener)->onEvent(android::String16("progress"), android::String16(body.c_str())).isOk()) ++listener;
        else listener = g_progress_listeners.erase(listener);
    }
}

void PublishCompletion(const FlatpakResult& result) {
    Json::Value event(Json::objectValue);
    event["phase"] = "complete";
    event["result"] = EncodeResult(result);
    const std::string body = Encode(event);
    std::lock_guard<std::mutex> guard(g_listener_mutex);
    auto listener = g_progress_listeners.begin();
    while (listener != g_progress_listeners.end()) {
        if ((*listener)->onEvent(android::String16("progress"), android::String16(body.c_str())).isOk()) ++listener;
        else listener = g_progress_listeners.erase(listener);
    }
}

void OnProgress(const char* line, void*) {
    PublishProgress(line ? line : "");
}

void OnComplete(const FlatpakResult* result, void*) {
    if (result) PublishCompletion(*result);
}

bool HasEmbeddedNul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

std::string ToUtf8(const android::String16& value) {
    return android::String8(value).c_str();
}

class LinuxdService final : public BnLinuxd {
  public:
    android::binder::Status call(const android::String16& command16,
                                 const android::String16& args16,
                                 android::String16* aidl_return) override {
        const std::string command = ToUtf8(command16);
        const std::string args = ToUtf8(args16);
        auto reply = [aidl_return](const std::string& value) {
            *aidl_return = android::String16(value.c_str());
        };
        Json::Value request;
        if (!Parse(args.empty() ? "{}" : args, &request)) {
            reply(Encode(Error("arguments must be a JSON object")));
            return android::binder::Status::ok();
        }

        std::string ref_storage;
        std::string app_id_storage;
        const char* ref = nullptr;
        const char* app_id = nullptr;
        std::vector<std::string> run_arg_storage;
        std::vector<const char*> run_args;
        if (command == "install" || command == "uninstall") {
            if (!OnlyKeys(request, {"ref"}) || !request["ref"].isString() ||
                    HasEmbeddedNul(request["ref"].asString())) {
                reply(Encode(Error("a complete Flatpak ref is required")));
                return android::binder::Status::ok();
            }
            ref_storage = request["ref"].asString();
            ref = ref_storage.c_str();
        } else if (command == "run") {
            if (!OnlyKeys(request, {"appId", "args"}) || !request["appId"].isString() ||
                    HasEmbeddedNul(request["appId"].asString())) {
                reply(Encode(Error("a Flatpak app ID is required")));
                return android::binder::Status::ok();
            }
            app_id_storage = request["appId"].asString();
            app_id = app_id_storage.c_str();
            const Json::Value extra = request["args"];
            if (!extra.isNull() && !extra.isArray()) {
                reply(Encode(Error("args must be an array of strings")));
                return android::binder::Status::ok();
            }
            if (extra.isArray()) {
                if (extra.size() > 64) {
                    reply(Encode(Error("too many run arguments")));
                    return android::binder::Status::ok();
                }
                for (const auto& arg : extra) {
                    if (!arg.isString() || arg.asString().size() > 4096 ||
                            HasEmbeddedNul(arg.asString())) {
                        reply(Encode(Error("invalid run argument")));
                        return android::binder::Status::ok();
                    }
                    run_arg_storage.push_back(arg.asString());
                }
            }
            for (const std::string& arg : run_arg_storage) run_args.push_back(arg.c_str());
        } else if (command == "kill") {
            if (!OnlyKeys(request, {"appId"}) || !request["appId"].isString() ||
                    HasEmbeddedNul(request["appId"].asString())) {
                reply(Encode(Error("a Flatpak app ID is required")));
                return android::binder::Status::ok();
            }
            app_id_storage = request["appId"].asString();
            app_id = app_id_storage.c_str();
        } else if (!OnlyKeys(request, {})) {
            reply(Encode(Error("unexpected arguments")));
            return android::binder::Status::ok();
        }

        if (command == "install" || command == "uninstall") PublishProgress(command + " started");
        FlatpakResult result{};
        flatpak_manager_call(command.c_str(), ref, app_id, run_args.data(), run_args.size(), &result);
        reply(Encode(EncodeResult(result)));
        flatpak_manager_result_clear(&result);
        return android::binder::Status::ok();
    }

    android::binder::Status subscribe(const android::String16& topic16,
            const android::sp<ILinuxdListener>& listener) override {
        if (ToUtf8(topic16) != "progress" || !listener) return android::binder::Status::ok();
        std::lock_guard<std::mutex> guard(g_listener_mutex);
        if (std::find(g_progress_listeners.begin(), g_progress_listeners.end(), listener) ==
                g_progress_listeners.end()) g_progress_listeners.push_back(listener);
        return android::binder::Status::ok();
    }

    android::binder::Status unsubscribe(const android::String16& topic16,
            const android::sp<ILinuxdListener>& listener) override {
        const std::string topic = ToUtf8(topic16);
        std::lock_guard<std::mutex> guard(g_listener_mutex);
        if (topic == "progress") g_progress_listeners.erase(
                std::remove(g_progress_listeners.begin(), g_progress_listeners.end(), listener),
                g_progress_listeners.end());
        return android::binder::Status::ok();
    }
};
}  // namespace

int main() {
    flatpak_manager_init();
    flatpak_manager_set_callbacks(OnProgress, OnComplete, nullptr);
    android::sp<LinuxdService> service = new LinuxdService();
    if (android::defaultServiceManager()->addService(android::String16(kInstance), service) != android::OK)
        return 1;
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::ProcessState::self()->startThreadPool();
    android::IPCThreadState::self()->joinThreadPool();
    return 1;
}
