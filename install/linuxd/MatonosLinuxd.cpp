#include <org/matonos/systembridge/BnLinuxd.h>
#include <org/matonos/systembridge/ILinuxdListener.h>
#include <org/matonos/systembridge/ILinuxdIdentity.h>
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
#include <cstdio>
#include <utils/String8.h>

#include "FlatpakManager.h"
#include "../../linux/flatpak/linux-paths.h"
#include "FlatpakPublish.h"
#include "StubVerify.h"
#include "DnsForwarderSockets.h"
#include <dirent.h>
#include <android-base/unique_fd.h>
#include <android/log.h>
#include <cstring>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <ctime>
#include <map>
#include <thread>
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
using org::matonos::systembridge::ILinuxdIdentity;

static std::mutex g_identity_mutex;
static android::sp<ILinuxdIdentity> g_identity;
extern "C" int stub_identity_query(const char* command, int uid, const char* package,
        char* ref, size_t ref_size, char* remote, size_t remote_size) {
    android::sp<ILinuxdIdentity> identity;
    { std::lock_guard<std::mutex> lock(g_identity_mutex); identity = g_identity; }
    if (!identity) return -1;
    android::String16 response;
    // Clear the incoming stub's Binder identity for the outgoing bridge call.
    auto* state = android::IPCThreadState::self();
    const int64_t token = state->clearCallingIdentity();
    auto status = identity->query(android::String16(command), uid, android::String16(package), &response);
    state->restoreCallingIdentity(token);
    if (!status.isOk()) return -1;
    Json::Value value;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    const std::string json = android::String8(response).c_str();
    std::string errors;
    if (json.size() > 4096 || !reader->parse(json.data(), json.data() + json.size(), &value, &errors) ||
            !value.isObject() || !value["value"].isInt()) return -1;
    if (ref) {
        if (!value["ref"].isString() || !value["remote"].isString()) return -1;
        const std::string r = value["ref"].asString(), m = value["remote"].asString();
        if (r.size() >= ref_size || m.size() >= remote_size ||
                r.find('\0') != std::string::npos || m.find('\0') != std::string::npos) return -1;
        memcpy(ref, r.c_str(), r.size() + 1); memcpy(remote, m.c_str(), m.size() + 1);
    }
    return value["value"].asInt();
}

namespace {
constexpr char kInstance[] = "org.matonos.systembridge.ILinuxd/default";
constexpr size_t kInputLimit = 64 * 1024;
constexpr size_t kListenerLimit = 64;
std::mutex g_listener_mutex;
std::vector<android::sp<ILinuxdListener>> g_progress_listeners;
std::mutex g_event_mutex;
std::condition_variable g_event_condition;
std::deque<std::string> g_events;
/* Self installs/updates in flight: stub UIDs and per-operation completion state. */
std::mutex g_self_mutex;
std::set<int> g_self_busy;
struct SelfOperationState {
    int uid;
    std::string ref;
    bool complete = false;
    std::string result;
};
std::condition_variable g_self_condition;
std::map<std::string, std::shared_ptr<SelfOperationState>> g_self_operations;

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
    if (result.operation_id) {
        /* Self-started operations name their ref so the bridge can show the launcher. */
        std::lock_guard<std::mutex> guard(g_self_mutex);
        auto operation = g_self_operations.find(result.operation_id);
        if (operation != g_self_operations.end()) {
            event["result"]["ref"] = operation->second->ref;
            event["result"]["uid"] = operation->second->uid;
            g_self_busy.erase(operation->second->uid);
            operation->second->result = Encode(event["result"]);
            operation->second->complete = true;
            g_self_operations.erase(operation);
            g_self_condition.notify_all();
        }
    }
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

bool ValidName(const std::string& value, size_t limit) {
    if (value.empty() || value.size() > limit) return false;
    for (char c : value)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

void PublishSelfFailure(const std::string& operation_id, const std::string& ref, int uid, const std::string& error) {
    Json::Value event(Json::objectValue);
    event["phase"] = "complete";
    event["result"]["ok"] = false;
    event["result"]["error"] = error;
    event["result"]["operationId"] = operation_id;
    event["result"]["ref"] = ref;
    event["result"]["uid"] = uid;
    NotifyListeners(Encode(event));
}

/*
 * "install me" / "update me", called by the generated stub itself with its
 * package name. The UID comes from binder and must own that package, which
 * must be signed with the per-device stub key; ref and remote come from its
 * manifest.
 * Staging (the network part) runs in this call, as the stub UID, while the
 * stub's activity is in the foreground; publishing continues asynchronously.
 */
std::string SelfOperation(bool update, const std::string& package, const std::string& operation_in) {
    const int uid = static_cast<int>(android::IPCThreadState::self()->getCallingUid());
    if (!operation_in.empty() && !ValidName(operation_in, 64)) return Encode(Error("invalid operation ID"));
    char error[512] = {0}, ref_buffer[1100], remote_buffer[130];
    if (stub_verify_caller(uid, package.c_str(), ref_buffer, sizeof(ref_buffer),
            remote_buffer, sizeof(remote_buffer), error, sizeof(error))) {
        fprintf(stderr, "matonos-linuxd: rejected self install from uid %d: %s\n", uid, error);
        return Encode(Error(error));
    }
    const std::string ref = ref_buffer;
    const std::string remote = remote_buffer[0] ? std::string(remote_buffer) : std::string("flathub");
    if (!flatpak_manager_valid_ref(ref.c_str()) || ref.compare(0, 4, "app/") != 0 || !ValidName(remote, 64))
        return Encode(Error("stub declares an invalid ref or remote"));
    const bool installed = flatpak_manager_installed_for_uid(ref.c_str(), uid) != 0;
    if (!update && installed) {
        Json::Value out; out["ok"] = true; out["installed"] = true;
        return Encode(out);
    }
    if (update && !installed) return Encode(Error("not installed"));
    const int runtime_uid = stub_runtime_uid(uid);
    if (runtime_uid < 10000 || runtime_uid == uid) return Encode(Error("Linux runtime app missing"));
    const std::string operation_id = !operation_in.empty() ? operation_in :
            std::string(update ? "update-" : "install-") + std::to_string(uid) + "-" + std::to_string(time(nullptr));
    auto operation = std::make_shared<SelfOperationState>();
    operation->uid = uid;
    operation->ref = ref;
    {
        std::lock_guard<std::mutex> guard(g_self_mutex);
        if (g_self_operations.count(operation_id)) return Encode(Error("operation ID already in use"));
        if (!g_self_busy.insert(uid).second) {
            Json::Value out; out["ok"] = true; out["accepted"] = true; out["running"] = true;
            return Encode(out);
        }
        g_self_operations[operation_id] = operation;
    }
    auto fail = [&](const std::string& message) {
        {
            std::lock_guard<std::mutex> guard(g_self_mutex);
            g_self_busy.erase(uid);
            g_self_operations.erase(operation_id);
        }
        PublishSelfFailure(operation_id, ref, uid, message);
        return Encode(Error(message));
    };
    PublishProgress(std::string(update ? "update" : "install") + " started");
    std::vector<char> app_commit(65), runtime_ref(512), runtime_commit(65), metadata(16384), desktop(131072);
    if (flatpak_manager_prepare(ref.c_str(), remote.c_str(), uid, runtime_uid, operation_id.c_str(),
            app_commit.data(), app_commit.size(), runtime_ref.data(), runtime_ref.size(),
            runtime_commit.data(), runtime_commit.size(), metadata.data(), metadata.size(),
            desktop.data(), desktop.size(), error, sizeof(error)))
        return fail(error[0] ? error : "cannot prepare Flatpak install");
    FlatpakResult result{};
    flatpak_manager_call("install", ref.c_str(), 0, operation_id.c_str(), app_commit.data(),
            runtime_ref.data(), runtime_commit.data(), remote.c_str(), uid, runtime_uid, &result);
    const bool accepted = result.ok && result.accepted;
    const std::string message = result.error ? result.error : "Flatpak install was rejected";
    flatpak_manager_result_clear(&result);
    if (!accepted) return fail(message);
    // The stub owns the foreground service and wake lock. Keep its Binder
    // call alive through download AND asynchronous deployment, including
    // failure/cancellation, rather than releasing them on "accepted".
    std::unique_lock<std::mutex> lock(g_self_mutex);
    g_self_condition.wait(lock, [&] { return operation->complete; });
    return operation->result;
}

bool HexDecode(const std::string& hex, std::vector<unsigned char>* out) {
    if (hex.empty() || hex.size() % 2 || hex.size() > 16384) return false;
    out->clear();
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned value = 0;
        for (size_t j = i; j < i + 2; ++j) {
            char c = hex[j];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else return false;
        }
        out->push_back(static_cast<unsigned char>(value));
    }
    return true;
}

class LinuxdService final : public BnLinuxd {
  public:
    android::binder::Status registerIdentity(const android::sp<ILinuxdIdentity>& identity) override {
        if (!IsTrustedCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        std::lock_guard<std::mutex> lock(g_identity_mutex);
        g_identity = identity;
        return android::binder::Status::ok();
    }
    android::binder::Status downloadsRequest(const android::String16& packageName,
            android::String16* aidl_return) override {
        const int uid = android::IPCThreadState::self()->getCallingUid();
        char error[512] = {}, ref[1100], remote[130];
        FlatpakResult result = {};
        if (stub_verify_caller(uid, ToUtf8(packageName).c_str(), ref, sizeof(ref),
                remote, sizeof(remote), error, sizeof(error))) {
            *aidl_return = android::String16(Encode(Error("unverified stub")).c_str());
        } else {
            flatpak_manager_downloads_request(ref, uid, &result);
            *aidl_return = android::String16(Encode(EncodeResult(result)).c_str());
            flatpak_manager_result_clear(&result);
        }
        return android::binder::Status::ok();
    }

    android::binder::Status answerDownloads(const android::String16& packageName,
            const android::String16& request, bool allow,
            android::String16* aidl_return) override {
        const int uid = android::IPCThreadState::self()->getCallingUid();
        const int pid = android::IPCThreadState::self()->getCallingPid();
        char error[512] = {}, ref[1100], remote[130];
        Json::Value out;
        if (stub_verify_caller(uid, ToUtf8(packageName).c_str(), ref, sizeof(ref),
                remote, sizeof(remote), error, sizeof(error)) ||
                flatpak_manager_downloads_answer(ref, uid, pid, ToUtf8(request).c_str(), allow))
            out = Error("Downloads request changed or stub is unavailable");
        else out["ok"] = true;
        *aidl_return = android::String16(Encode(out).c_str());
        return android::binder::Status::ok();
    }

    android::binder::Status installSelf(const android::String16& packageName,
            const android::String16& operationId, android::String16* aidl_return) override {
        *aidl_return = android::String16(SelfOperation(false, ToUtf8(packageName), ToUtf8(operationId)).c_str());
        return android::binder::Status::ok();
    }

    android::binder::Status updateSelf(const android::String16& packageName,
            const android::String16& operationId, android::String16* aidl_return) override {
        *aidl_return = android::String16(SelfOperation(true, ToUtf8(packageName), ToUtf8(operationId)).c_str());
        return android::binder::Status::ok();
    }

    android::binder::Status prepareRuntime(const android::String16& packageName,
            android::String16* aidl_return) override {
        const int uid = static_cast<int>(android::IPCThreadState::self()->getCallingUid());
        char error[512] = {0}, ref_buffer[1100], remote_buffer[130];
        if (stub_verify_caller(uid, ToUtf8(packageName).c_str(), ref_buffer, sizeof(ref_buffer),
                remote_buffer, sizeof(remote_buffer), error, sizeof(error))) {
            *aidl_return = android::String16(Encode(Error(error[0] ? error : "unverified stub")).c_str());
            return android::binder::Status::ok();
        }
        if (flatpak_manager_prepare_runtime(uid)) {
            *aidl_return = android::String16(Encode(Error("cannot prepare runtime directory")).c_str());
            return android::binder::Status::ok();
        }
        Json::Value out; out["ok"] = true;
        *aidl_return = android::String16(Encode(out).c_str());
        return android::binder::Status::ok();
    }

    android::binder::Status createDnsForwarderSockets(const android::String16& address16,
            int32_t stubUid,
            std::vector<android::os::ParcelFileDescriptor>* sockets) override {
        if (!IsTrustedCaller() || stubUid < 10000 || sockets == nullptr)
            return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        int udp = -1, tcp = -1;
        int result = matonos_dns_create_sockets(ToUtf8(address16).c_str(),
                static_cast<uint32_t>(stubUid), &udp, &tcp);
        if (result != 0)
            return android::binder::Status::fromServiceSpecificError(-result,
                    "Cannot bind per-app DNS sockets");
        sockets->clear();
        sockets->emplace_back(android::base::unique_fd(udp));
        sockets->emplace_back(android::base::unique_fd(tcp));
        return android::binder::Status::ok();
    }

    android::binder::Status launchSpec(const android::String16& ref,
            const android::String16& dnsServers,
            const std::optional<android::os::ParcelFileDescriptor>& x11Directory,
            const std::optional<android::String16>& x11Display,
            bool gameControllers, int32_t stubUid, int32_t stubPid,
            const android::os::ParcelFileDescriptor& lifeline,
            android::String16* aidl_return) override {
        if (!IsTrustedCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        FlatpakResult result = {};
        flatpak_manager_launch_spec(ToUtf8(ref).c_str(), ToUtf8(dnsServers).c_str(), x11Directory ? x11Directory->get() : -1, x11Display ? ToUtf8(*x11Display).c_str() : nullptr, gameControllers, stubUid, stubPid, lifeline.get(), &result);
        Json::Value spec;
        if (result.ok && result.output && Parse(result.output, &spec)) {
            spec["ok"] = true;
            *aidl_return = android::String16(Encode(spec).c_str());
        } else {
            *aidl_return = android::String16(Encode(EncodeResult(result)).c_str());
        }
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
            MatonPathScan dir;unsigned uid;
            if(maton_scan_open(&dir,"install")) {reply(Encode(Error("Linux data inventory unavailable")));return android::binder::Status::ok();}
            while(maton_scan_next(&dir,&uid)>0) out["uids"].append(static_cast<int>(uid));
            maton_scan_close(&dir);reply(Encode(out));return android::binder::Status::ok();
        }
        if (command == "set_stub_signer") {
            /* The bridge's per-device stub certificate, checked on "install me". */
            std::vector<unsigned char> der;
            if (!OnlyKeys(request, {"certificate"}) || !request["certificate"].isString() ||
                    !HexDecode(request["certificate"].asString(), &der) ||
                    stub_store_signer(der.data(), der.size())) {
                reply(Encode(Error("cannot store stub certificate")));
                return android::binder::Status::ok();
            }
            Json::Value out; out["ok"] = true;
            reply(Encode(out));
            return android::binder::Status::ok();
        }
        if (command == "stage") {
            /* Stage only after the stub exists: the pull runs as the stub UID
             * (installerUid), so its network use and DNS are the stub's. */
            if (!OnlyKeys(request, {"ref", "remote", "installerUid", "runtimeUid", "operationId"}) || !request["ref"].isString() ||
                    !request["installerUid"].isInt() || !request["runtimeUid"].isInt() || !request["operationId"].isString() ||
                    HasEmbeddedNul(request["ref"].asString()) ||
                    (request.isMember("remote") && (!request["remote"].isString() ||
                            HasEmbeddedNul(request["remote"].asString())))) {
                reply(Encode(Error("a Flatpak ref is required")));
                return android::binder::Status::ok();
            }
            const std::string ref = request["ref"].asString();
            const std::string remote = request.isMember("remote") ? request["remote"].asString() : std::string("flathub");
            std::vector<char> app_commit(65), runtime_ref(512), runtime_commit(65), metadata(16384), desktop(131072), error(512);
            if (flatpak_manager_prepare(ref.c_str(), remote.c_str(), request["installerUid"].asInt(),
                    request["runtimeUid"].asInt(), request["operationId"].asCString(),
                    app_commit.data(), app_commit.size(), runtime_ref.data(), runtime_ref.size(),
                    runtime_commit.data(), runtime_commit.size(), metadata.data(), metadata.size(),
                    desktop.data(), desktop.size(), error.data(), error.size())) {
                reply(Encode(Error(error[0] ? error.data() : "cannot prepare Flatpak install")));
                return android::binder::Status::ok();
            }
            Json::Value out; out["ok"] = true;
            out["ref"] = ref;
            out["appCommit"] = app_commit.data();
            out["runtimeRef"] = runtime_ref.data();
            out["runtimeCommit"] = runtime_commit.data();
            out["metadata"] = metadata.data();
            out["desktopEntry"] = desktop.data();
            reply(Encode(out));
            return android::binder::Status::ok();
        }
        std::string ref_storage;
        const char* ref = nullptr;
        int delete_data = 0;
        std::string app_commit_storage, runtime_ref_storage, runtime_commit_storage, remote_storage;
        const char* app_commit = nullptr;
        const char* runtime_ref = nullptr;
        const char* runtime_commit = nullptr;
        const char* remote = nullptr;
        int app_uid = -1, runtime_uid = -1;
        std::string operation_id_storage;
        const char* operation_id = nullptr;
        if (command == "install" || command == "uninstall" || command == "kill" || command == "installed_for_uid" || (command == "metadata" || command == "desktop_entry" || command == "icon" || command == "launch_status")) {
            const bool uninstall = command == "uninstall";
            const bool install = command == "install";
            const bool installed_for_uid = command == "installed_for_uid";
            if (!(uninstall ? OnlyKeys(request, {"ref", "uid", "deleteData", "operationId"}) :
                    install ? OnlyKeys(request, {"ref", "operationId", "appCommit", "runtimeRef", "runtimeCommit", "remote", "uid", "runtimeUid"}) :
                    installed_for_uid ? OnlyKeys(request, {"ref", "uid"}) :
                    OnlyKeys(request, {"ref", "uid", "operationId"})) || !request["ref"].isString() ||
                    (install && (!request["appCommit"].isString() || !request["runtimeRef"].isString() ||
                        !request["runtimeCommit"].isString() || !request["remote"].isString() ||
                        !request["uid"].isInt() || !request["runtimeUid"].isInt())) ||
                    (!request["uid"].isInt() || request["uid"].asInt() < 10000) ||
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
            app_uid = request["uid"].asInt();
            if (install) {
                app_commit_storage = request["appCommit"].asString(); app_commit = app_commit_storage.c_str();
                runtime_ref_storage = request["runtimeRef"].asString(); runtime_ref = runtime_ref_storage.c_str();
                runtime_commit_storage = request["runtimeCommit"].asString(); runtime_commit = runtime_commit_storage.c_str();
                remote_storage = request["remote"].asString(); remote = remote_storage.c_str();
                app_uid = request["uid"].asInt(); runtime_uid = request["runtimeUid"].asInt();
                if (app_uid < 10000 || runtime_uid < 10000 || app_uid == runtime_uid ||
                        app_uid / 100000 != runtime_uid / 100000) {
                    reply(Encode(Error("valid same-user app and runtime package UIDs are required")));
                    return android::binder::Status::ok();
                }
            }
            delete_data = request.get("deleteData", false).asBool() ? 1 : 0;
            if (request.isMember("operationId")) { operation_id_storage = request["operationId"].asString(); operation_id = operation_id_storage.c_str(); }
        } else if (command == "list_installed") {
            if (!OnlyKeys(request, {"userId"}) || !request["userId"].isInt() ||
                    request["userId"].asInt() < 0 || request["userId"].asInt() > INT_MAX / 100000) {
                reply(Encode(Error("a valid Android user is required")));
                return android::binder::Status::ok();
            }
            app_uid = request["userId"].asInt();
        } else if (command == "list_remotes" || command == "add_flathub") {
            /* The bridge supplies the preinstalled runtime app's UID so remote
             * setup targets that app's --system installation. */
            if (!OnlyKeys(request, {"runtimeUid"}) ||
                    (request.isMember("runtimeUid") && (!request["runtimeUid"].isInt() ||
                            request["runtimeUid"].asInt() < 10000))) {
                reply(Encode(Error("a valid runtimeUid is required")));
                return android::binder::Status::ok();
            }
            if (request.isMember("runtimeUid")) runtime_uid = request["runtimeUid"].asInt();
        } else if (!OnlyKeys(request, {})) {
            reply(Encode(Error("unexpected arguments")));
            return android::binder::Status::ok();
        }

        if (command == "install" || command == "uninstall") PublishProgress(command + " started");
        FlatpakResult result{};
        flatpak_manager_call(command.c_str(), ref, delete_data,
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

namespace {
void* MatonStderrLogger(void* arg) {
    FILE* stream = static_cast<FILE*>(arg);
    char line[4096];
    while (fgets(line, sizeof(line), stream)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        __android_log_write(ANDROID_LOG_INFO, "matonos-linuxd", line);
    }
    return nullptr;
}
void RedirectStderrToLog() {
    int fds[2];
    if (pipe(fds) != 0) return;
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    FILE* in = fdopen(fds[0], "r");
    if (!in) { close(fds[0]); return; }
    setvbuf(stderr, nullptr, _IOLBF, 0);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, MatonStderrLogger, in) == 0) pthread_detach(thread);
}
}  // namespace

int main() {
    /* init sends service stdio to /dev/null, so route stderr to logcat. */
    RedirectStderrToLog();
    /* Leftovers from a killed install; never block startup on them. */
    if (flatpak_cleanup_stale_staging()) fprintf(stderr, "matonos-linuxd: stale staging cleanup incomplete\n");
    flatpak_manager_init();
    flatpak_manager_reconcile_runtime_refs();
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
