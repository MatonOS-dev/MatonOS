#include <org/matonos/systembridge/BnLinuxd.h>
#include <org/matonos/systembridge/ILinuxdListener.h>
#include <binder/IServiceManager.h>
#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <binder/IPCThreadState.h>
#include <binder/PermissionCache.h>
#include <binder/IPermissionController.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <binder/ParcelFileDescriptor.h>
#include <json/json.h>
#include <utils/String8.h>

#include "FlatpakManager.h"
#include <dirent.h>
#include <android-base/unique_fd.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

using org::matonos::systembridge::BnLinuxd;
using org::matonos::systembridge::ILinuxdListener;

namespace {
constexpr char kInstance[] = "org.matonos.systembridge.ILinuxd/default";
constexpr size_t kInputLimit = 64 * 1024;
constexpr size_t kListenerLimit = 64;
std::mutex g_listener_mutex;
std::vector<android::sp<ILinuxdListener>> g_progress_listeners;
std::mutex g_event_mutex;
std::condition_variable g_event_condition;
std::deque<std::string> g_events;

class ListenerDeathRecipient final : public android::IBinder::DeathRecipient {
  public:
    void binderDied(const android::wp<android::IBinder>& who) override {
        std::lock_guard<std::mutex> guard(g_listener_mutex);
        g_progress_listeners.erase(std::remove_if(g_progress_listeners.begin(), g_progress_listeners.end(),
                [&who](const android::sp<ILinuxdListener>& listener) {
                    return android::IInterface::asBinder(listener).get() == who.unsafe_get();
                }), g_progress_listeners.end());
    }
};
android::sp<ListenerDeathRecipient> g_listener_death = new ListenerDeathRecipient();

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

void* DispatchEvents(void*) {
    for (;;) {
        std::string body;
        {
            std::unique_lock<std::mutex> lock(g_event_mutex);
            g_event_condition.wait(lock, [] { return !g_events.empty(); });
            body = std::move(g_events.front());
            g_events.pop_front();
        }
        std::vector<android::sp<ILinuxdListener>> listeners;
        { std::lock_guard<std::mutex> guard(g_listener_mutex); listeners = g_progress_listeners; }
        std::vector<android::sp<ILinuxdListener>> dead;
        for (const auto& listener : listeners)
            if (!listener->onEvent(android::String16("progress"), android::String16(body.c_str())).isOk()) dead.push_back(listener);
        if (!dead.empty()) {
            for (const auto& listener : dead) android::IInterface::asBinder(listener)->unlinkToDeath(g_listener_death);
            std::lock_guard<std::mutex> guard(g_listener_mutex);
            g_progress_listeners.erase(std::remove_if(g_progress_listeners.begin(), g_progress_listeners.end(),
                    [&dead](const android::sp<ILinuxdListener>& listener) {
                        return std::find(dead.begin(), dead.end(), listener) != dead.end();
                    }), g_progress_listeners.end());
        }
    }
    return nullptr;
}

void NotifyListeners(const std::string& body) {
    {
        std::lock_guard<std::mutex> guard(g_event_mutex);
        if (g_events.size() >= 256) g_events.pop_front();
        g_events.push_back(body);
    }
    g_event_condition.notify_one();
}

/*
 * Binder callbacks run on this independent worker. A stalled bridge listener
 * cannot hold the Flatpak operation mutex or delay draining the CLI pipe.
 */
bool StartEventDispatcher() {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, DispatchEvents, nullptr) != 0) return false;
    (void)pthread_detach(thread);
    return true;
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
    if (result.operation_id) value["operationId"] = result.operation_id;
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
                percent = std::min(100u, percent > 10u ? 100u : percent * 10u + static_cast<unsigned>(text[j] - '0'));
            event["percent"] = percent;
        }
        break;
    }
    const std::string body = Encode(event);
    NotifyListeners(body);
}

void PublishCompletion(const FlatpakResult& result) {
    Json::Value event(Json::objectValue);
    event["phase"] = "complete";
    event["result"] = EncodeResult(result);
    const std::string body = Encode(event);
    NotifyListeners(body);
}

bool IsTrustedCaller() {
    android::IPCThreadState* state = android::IPCThreadState::self();
    if (!android::PermissionCache::checkPermission(android::String16("org.matonos.permission.SYSTEM_BRIDGE"),
            state->getCallingPid(), state->getCallingUid())) return false;
    // All operations must pass through the bridge's certificate/target gate.
    android::sp<android::IPermissionController> permissions =
            android::interface_cast<android::IPermissionController>(
                    android::defaultServiceManager()->checkService(android::String16("permission")));
    int bridgeUid = permissions ? permissions->getPackageUid(
            android::String16("org.matonos.systembridge"), 0) : -1;
    return bridgeUid >= 0 && state->getCallingUid() == static_cast<uid_t>(bridgeUid);
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
    android::binder::Status launchGraphical(const android::String16& ref,
            const android::os::ParcelFileDescriptor& runtimeDirectory,
            const android::String16& dnsServers,
            const std::optional<android::os::ParcelFileDescriptor>& x11Directory,
            const std::optional<android::String16>& x11Display,
            bool gameControllers, int32_t stubUid, int32_t stubPid,
            const android::os::ParcelFileDescriptor& lifeline,
            android::String16* aidl_return) override {
        if (!IsTrustedCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        FlatpakResult result = {};
        flatpak_manager_launch_graphical(ToUtf8(ref).c_str(), runtimeDirectory.get(), ToUtf8(dnsServers).c_str(), x11Directory ? x11Directory->get() : -1, x11Display ? ToUtf8(*x11Display).c_str() : nullptr, gameControllers, stubUid, stubPid, lifeline.get(), &result);
        *aidl_return = android::String16(Encode(EncodeResult(result)).c_str());
        flatpak_manager_result_clear(&result);
        return android::binder::Status::ok();
    }

    android::binder::Status call(const android::String16& command16,
                                 const android::String16& args16,
                                 android::String16* aidl_return) override {
        if (!IsTrustedCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
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

        if (command == "delete_linux_data") {
            if(!OnlyKeys(request,{"uid"}) || !request["uid"].isInt()) reply(Encode(Error("uid required")));
            else if(flatpak_manager_delete_data(request["uid"].asInt())) reply(Encode(Error("Linux data deletion failed")));
            else { Json::Value ok;ok["ok"]=true;reply(Encode(ok)); }
            return android::binder::Status::ok();
        }
        if(command == "linux_data_uids") {
            Json::Value out;out["ok"]=true;out["uids"]=Json::Value(Json::arrayValue);
            DIR* dir=opendir("/data/matonos/linux/apps");
            if(!dir) { reply(Encode(Error("Linux data inventory unavailable")));return android::binder::Status::ok(); }
            while(auto* entry=readdir(dir)) {
                char* end=nullptr;long uid=strtol(entry->d_name,&end,10);
                if(*entry->d_name && (!*end || !strcmp(end,".owner")) && uid>=10000 && uid<=INT32_MAX && uid%100000<=19999 && uid%100000>=10000)
                    out["uids"].append(static_cast<int>(uid));
            }
            closedir(dir);reply(Encode(out));return android::binder::Status::ok();
        }
        std::string ref_storage;
        std::string app_id_storage;
        const char* ref = nullptr;
        const char* app_id = nullptr;
        int delete_data = 0;
        std::string app_commit_storage, runtime_ref_storage, runtime_commit_storage, remote_storage;
        const char* app_commit = nullptr;
        const char* runtime_ref = nullptr;
        const char* runtime_commit = nullptr;
        const char* remote = nullptr;
        int app_uid = -1, runtime_uid = -1;
        std::string operation_id_storage;
        const char* operation_id = nullptr;
        std::vector<std::string> run_arg_storage;
        std::vector<const char*> run_args;
        if (command == "install" || command == "uninstall" || (command == "metadata" || command == "desktop_entry" || command == "icon" || command == "launch_status")) {
            const bool uninstall = command == "uninstall";
            const bool install = command == "install";
            if (!(uninstall ? OnlyKeys(request, {"ref", "deleteData", "operationId"}) :
                    install ? OnlyKeys(request, {"ref", "operationId", "appCommit", "runtimeRef", "runtimeCommit", "remote", "uid", "runtimeUid"}) :
                    OnlyKeys(request, {"ref", "operationId"})) || !request["ref"].isString() ||
                    (install && (!request["appCommit"].isString() || !request["runtimeRef"].isString() ||
                        !request["runtimeCommit"].isString() || !request["remote"].isString() ||
                        !request["uid"].isInt() || !request["runtimeUid"].isInt())) ||
                    (install && (HasEmbeddedNul(request["appCommit"].asString()) ||
                        HasEmbeddedNul(request["runtimeRef"].asString()) ||
                        HasEmbeddedNul(request["runtimeCommit"].asString()) ||
                        HasEmbeddedNul(request["remote"].asString()))) ||
                    (request.isMember("deleteData") && !request["deleteData"].isBool()) ||
                    (request.isMember("operationId") && (!request["operationId"].isString() || request["operationId"].asString().size() > 128 || HasEmbeddedNul(request["operationId"].asString()))) ||
                    HasEmbeddedNul(request["ref"].asString())) {
                reply(Encode(Error("a complete Flatpak ref is required")));
                return android::binder::Status::ok();
            }
            ref_storage = request["ref"].asString();
            ref = ref_storage.c_str();
            if (install) {
                app_commit_storage = request["appCommit"].asString(); app_commit = app_commit_storage.c_str();
                runtime_ref_storage = request["runtimeRef"].asString(); runtime_ref = runtime_ref_storage.c_str();
                runtime_commit_storage = request["runtimeCommit"].asString(); runtime_commit = runtime_commit_storage.c_str();
                remote_storage = request["remote"].asString(); remote = remote_storage.c_str();
                app_uid = request["uid"].asInt(); runtime_uid = request["runtimeUid"].asInt();
                if (app_uid < 10000 || runtime_uid < 10000 || app_uid == runtime_uid) {
                    reply(Encode(Error("valid app and runtime package UIDs are required")));
                    return android::binder::Status::ok();
                }
            }
            delete_data = request.get("deleteData", false).asBool() ? 1 : 0;
            if (request.isMember("operationId")) { operation_id_storage = request["operationId"].asString(); operation_id = operation_id_storage.c_str(); }
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
        flatpak_manager_call(command.c_str(), ref, app_id, run_args.data(), run_args.size(), delete_data,
                operation_id, app_commit, runtime_ref, runtime_commit, remote, app_uid, runtime_uid, &result);
        reply(Encode(EncodeResult(result)));
        flatpak_manager_result_clear(&result);
        return android::binder::Status::ok();
    }

    android::binder::Status subscribe(const android::String16& topic16,
            const android::sp<ILinuxdListener>& listener) override {
        if (!IsTrustedCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        if (ToUtf8(topic16) != "progress" || !listener) return android::binder::Status::ok();
        android::sp<ILinuxdListener> evicted;
        {
            std::lock_guard<std::mutex> guard(g_listener_mutex);
            if (std::find(g_progress_listeners.begin(), g_progress_listeners.end(), listener) ==
                    g_progress_listeners.end()) {
                if (android::IInterface::asBinder(listener)->linkToDeath(g_listener_death) != android::OK) return android::binder::Status::ok();
                g_progress_listeners.push_back(listener);
            }
            if (g_progress_listeners.size() > kListenerLimit) {
                evicted = g_progress_listeners.front();
                g_progress_listeners.erase(g_progress_listeners.begin());
            }
        }
        if (evicted) android::IInterface::asBinder(evicted)->unlinkToDeath(g_listener_death);
        return android::binder::Status::ok();
    }

    android::binder::Status unsubscribe(const android::String16& topic16,
            const android::sp<ILinuxdListener>& listener) override {
        if (!IsTrustedCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        const std::string topic = ToUtf8(topic16);
        bool removed = false;
        {
            std::lock_guard<std::mutex> guard(g_listener_mutex);
            if (topic == "progress") {
                removed = std::find(g_progress_listeners.begin(), g_progress_listeners.end(), listener) != g_progress_listeners.end();
                g_progress_listeners.erase(std::remove(g_progress_listeners.begin(), g_progress_listeners.end(), listener),
                        g_progress_listeners.end());
            }
        }
        if (removed) android::IInterface::asBinder(listener)->unlinkToDeath(g_listener_death);
        return android::binder::Status::ok();
    }
};
}  // namespace

int main() {
    flatpak_manager_init();
    flatpak_manager_set_callbacks(OnProgress, OnComplete, nullptr);
    if (!StartEventDispatcher()) return 1;
    android::sp<LinuxdService> service = new LinuxdService();
    if (android::defaultServiceManager()->addService(android::String16(kInstance), service) != android::OK)
        return 1;
    android::ProcessState::self()->setThreadPoolMaxThreadCount(4);
    android::ProcessState::self()->startThreadPool();
    android::IPCThreadState::self()->joinThreadPool();
    return 1;
}
