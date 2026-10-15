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
/* Never hold g_identity_mutex while waiting for a store mutation: existing
 * GC can ask the identity provider while holding the store lock. */
static std::mutex g_identity_lifecycle_mutex;
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

/* The bridge owns every staged authorization. Its death invalidates all
 * unpublished handoffs; cleanup waits for any active verification to finish. */
class IdentityDeathRecipient final : public android::IBinder::DeathRecipient {
  public:
    void binderDied(const android::wp<android::IBinder>& who) override {
        std::lock_guard<std::mutex> lifecycle(g_identity_lifecycle_mutex);
        bool lost=false;
        {
            std::lock_guard<std::mutex> lock(g_identity_mutex);
            if(g_identity && android::IInterface::asBinder(g_identity).get()==who.unsafe_get()) {
                g_identity.clear();lost=true;
            }
        }
        if(lost)flatpak_manager_abort_handoffs();
    }
};
android::sp<IdentityDeathRecipient> g_identity_death = new IdentityDeathRecipient();

namespace {
constexpr char kInstance[] = "org.matonos.systembridge.ILinuxd/default";
constexpr size_t kInputLimit = 64 * 1024;
constexpr size_t kListenerLimit = 64;
std::mutex g_listener_mutex;
std::vector<android::sp<ILinuxdListener>> g_progress_listeners;
std::mutex g_event_mutex;
std::condition_variable g_event_condition;
std::deque<std::string> g_events;

/* Root/shell-only diagnostic snapshots, bounded to the most recent operations. The
 * test harness uses this because the bridge shell allowlist has no status RPC. */
std::mutex g_snapshot_mutex;
std::map<std::string, Json::Value> g_snapshots;
thread_local std::string g_staging_operation;
void Snapshot(const std::string& id, const char* phase, const std::string& error = "") {
    if(id.empty())return;
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if(!g_snapshots.count(id) && g_snapshots.size()>=64) {
        auto old=std::find_if(g_snapshots.begin(),g_snapshots.end(),[](const auto& entry){return entry.second["phase"]=="complete";});
        if(old==g_snapshots.end())return;
        g_snapshots.erase(old);
    }
    auto& state=g_snapshots[id];state["operationId"]=id;state["phase"]=phase;
    state["sequence"]=state.get("sequence",0).asUInt64()+1;
    if(!error.empty())state["error"]=error;
    else state.removeMember("error");
}
thread_local std::string g_last_staging_progress;
void StagingProgress(const char* text) {
    std::string value=text;
    // Flatpak redraws an unchanged bar with a changing average speed/ETA.
    // Those redraws must not keep a stalled download alive indefinitely.
    auto percent=value.find('%');
    if(percent!=std::string::npos)value.resize(percent+1);
    if(value!=g_last_staging_progress) {
        g_last_staging_progress=value;
        Snapshot(g_staging_operation,"verifying");
    }
}

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

bool Parse(const std::string& text, Json::Value* root, size_t limit=kInputLimit) {
    if (text.size() > limit) return false;
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
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
    // Every refused request is logged: Binder callers often surface only a
    // generic failure, which made install problems invisible on device.
    __android_log_print(ANDROID_LOG_WARN, "matonos-linuxd", "request failed: %s", message.c_str());
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
    if(!g_staging_operation.empty())event["operationId"]=g_staging_operation;
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

void HandoffProgress(const char* text) {
    const std::string previous=g_last_staging_progress;
    StagingProgress(text);
    if(previous != g_last_staging_progress)PublishProgress(text);
}

void PublishCompletion(const FlatpakResult& result) {
    if(result.operation_id)Snapshot(result.operation_id,"complete",result.ok ? "" :
            (result.error ? result.error : "Flatpak publication failed"));
    Json::Value event(Json::objectValue);
    event["phase"] = "complete";
    event["result"] = EncodeResult(result);
    const std::string body = Encode(event);
    NotifyListeners(body);
}

int PackageUid(const char* package) {
    android::sp<android::IPermissionController> permissions =
            android::interface_cast<android::IPermissionController>(
                    android::defaultServiceManager()->checkService(android::String16("permission")));
    return permissions ? permissions->getPackageUid(android::String16(package), 0) : -1;
}

bool IsSubsystemCaller() {
    const uid_t callingUid = android::IPCThreadState::self()->getCallingUid();
    // PermissionController resolves the package's appId in user 0. Android
    // assigns the same appId to that package for each user, so reconstruct the
    // expected per-user UID from the Binder caller instead of authorizing only
    // the primary user's subsystem process.
    const int userId = static_cast<int>(callingUid / 100000);
    const int baseUid = PackageUid("org.matonos.linux");
    if (baseUid < 0 || userId < 0) return false;
    const uid_t expectedUid = static_cast<uid_t>(userId * 100000 + baseUid % 100000);
    return callingUid == expectedUid;
}

bool IsStoreCommand(const std::string& command) {
    return command == "list_installed" || command == "list_remotes" || command == "add_flathub" ||
            command == "install_repo" || command == "remove_repo" || command == "install" ||
            command == "uninstall" || command == "installed_for_uid" || command == "check_update" ||
            command == "installed_commit" || command == "metadata" || command == "desktop_entry" ||
            command == "icon" || command == "begin_staged_install" || command == "verify_staged_install" ||
            command == "abort_staged_install" || command == "abort_pending" || command == "pending_metadata" ||
            command == "pending_desktop" || command == "pending_icon" || command == "bind_pending" ||
            command == "install_complete" || command == "install_failed";
}

bool IsTrustedCaller() {
    android::IPCThreadState* state = android::IPCThreadState::self();
    if (!android::PermissionCache::checkPermission(android::String16("org.matonos.permission.SYSTEM_BRIDGE"),
            state->getCallingPid(), state->getCallingUid())) return false;
    int bridgeUid = PackageUid("org.matonos.systembridge");
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

template<size_t N> bool ManifestString(const Json::Value& value,char (&out)[N]) {
    if(!value.isString())return false;
    const std::string text=value.asString();
    if(text.empty() || text.size()>=N || HasEmbeddedNul(text))return false;
    memcpy(out,text.c_str(),text.size()+1);return true;
}
bool ManifestCommit(const Json::Value& value,char (&out)[65]) {
    if(!ManifestString(value,out) || strlen(out)!=64)return false;
    for(const char* p=out;*p;p++)if(!(*p>='0' && *p<='9') && !(*p>='a' && *p<='f'))return false;
    return true;
}
bool StagedManifest(const Json::Value& value,FlatpakStagedManifest* out) {
    if(!value.isObject() || !OnlyKeys(value,{"version","operationId","appRef","appCommit","remote","deployments"}) ||
            (value["version"].type()!=Json::intValue && value["version"].type()!=Json::uintValue) ||
            !value["version"].isInt() || value["version"].asInt()!=1 ||
            !ManifestString(value["operationId"],out->operation_id) || !ValidName(out->operation_id,64) ||
            !ManifestString(value["appRef"],out->app_ref) || !flatpak_manager_valid_ref(out->app_ref) ||
            strncmp(out->app_ref,"app/",4) || !ManifestCommit(value["appCommit"],out->app_commit) ||
            !ManifestString(value["remote"],out->remote) ||
            !value["deployments"].isArray() || value["deployments"].empty() || value["deployments"].size()>64)return false;
    out->version=1;out->deployment_count=value["deployments"].size();
    std::set<std::string> refs;
    for(Json::ArrayIndex i=0;i<value["deployments"].size();i++) {
        const auto& d=value["deployments"][i];auto& target=out->deployments[i];
        if(!d.isObject() || !OnlyKeys(d,{"ref","commit","subpaths"}) ||
                !ManifestString(d["ref"],target.ref) || !flatpak_manager_valid_ref(target.ref) ||
                !refs.insert(target.ref).second || !ManifestCommit(d["commit"],target.commit) ||
                !d["subpaths"].isArray() || d["subpaths"].size()>32)return false;
        target.subpath_count=d["subpaths"].size();
        std::set<std::string> paths;
        for(Json::ArrayIndex j=0;j<d["subpaths"].size();j++) {
            if(!ManifestString(d["subpaths"][j],target.subpaths[j]))return false;
            std::string path=target.subpaths[j];
            if(path=="/" || path.back()=='/')return false;
            /* Stock Flatpak locale subpaths use /en. The leading slash is
             * relative to this deployment, never a caller filesystem path. */
            size_t start=path[0]=='/'?1:0;
            if(!paths.insert(path.substr(start)).second)return false;
            do {
                size_t end=path.find('/',start);
                std::string part=path.substr(start,end==std::string::npos?end:end-start);
                if(part.empty() || part=="." || part=="..")return false;
                if(end==std::string::npos)break;
                start=end+1;
            }while(true);
        }
    }
    return true;
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
    android::status_t dump(int fd, const android::Vector<android::String16>&) override {
        auto uid=android::IPCThreadState::self()->getCallingUid();
        if(uid!=0 && uid!=2000)return android::PERMISSION_DENIED;
        Json::Value states(Json::objectValue);
        { std::lock_guard<std::mutex> lock(g_snapshot_mutex);
          for(const auto& entry:g_snapshots)states[entry.first]=entry.second; }
        std::string body=Encode(states)+"\n";
        size_t offset=0;
        while(offset<body.size()) {
            ssize_t n=write(fd,body.data()+offset,body.size()-offset);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)return android::UNKNOWN_ERROR;
            offset+=static_cast<size_t>(n);
        }
        return android::OK;
    }
    android::binder::Status registerIdentity(const android::sp<ILinuxdIdentity>& identity) override {
        if (!IsTrustedCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        if(!identity)return android::binder::Status::fromExceptionCode(android::binder::Status::EX_ILLEGAL_ARGUMENT);
        std::lock_guard<std::mutex> lifecycle(g_identity_lifecycle_mutex);
        android::sp<ILinuxdIdentity> previous;
        auto next=android::IInterface::asBinder(identity);
        {
            std::lock_guard<std::mutex> lock(g_identity_mutex);
            previous=g_identity;
            if(previous && android::IInterface::asBinder(previous)==next)return android::binder::Status::ok();
            g_identity.clear();
        }
        if(previous) {
            android::IInterface::asBinder(previous)->unlinkToDeath(g_identity_death);
            flatpak_manager_abort_handoffs();
        }
        if(next->linkToDeath(g_identity_death)!=android::OK)
            return android::binder::Status::fromExceptionCode(android::binder::Status::EX_ILLEGAL_STATE);
        {std::lock_guard<std::mutex> lock(g_identity_mutex);g_identity=identity;}
        return android::binder::Status::ok();
    }
    android::binder::Status call(const android::String16& command16,
                                 const android::String16& args16,
                                 android::String16* aidl_return) override {
        const std::string command = ToUtf8(command16);
        if (IsStoreCommand(command) ? !IsSubsystemCaller() : !IsTrustedCaller())
            return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
        const std::string args = ToUtf8(args16);
        auto reply = [aidl_return](const std::string& value) {
            *aidl_return = android::String16(value.c_str());
        };
        Json::Value request;
        if (!Parse(args.empty() ? "{}" : args, &request, command=="install_repo"?128*1024:kInputLimit)) {
            reply(Encode(Error("arguments must be a JSON object")));
            return android::binder::Status::ok();
        }

        if(command=="begin_staged_install" || command=="verify_staged_install" || command=="abort_staged_install") {
            const bool begin=command=="begin_staged_install",verify=command=="verify_staged_install";
            if(!OnlyKeys(request,begin?std::initializer_list<const char*>{"operationId","ref","remote","compositorUid"}:
                    verify?std::initializer_list<const char*>{"operationId","manifest"}:std::initializer_list<const char*>{"operationId"}) ||
                    !request["operationId"].isString() || !ValidName(request["operationId"].asString(),64)) {
                reply(Encode(Error("invalid staged operation")));return android::binder::Status::ok();
            }
            std::string ref,remote;int uid=-1;
            if(begin) {
                if(!request["ref"].isString() || HasEmbeddedNul(request["ref"].asString()) ||
                        !request["remote"].isString() || HasEmbeddedNul(request["remote"].asString()) ||
                        !request["compositorUid"].isInt()) {
                    reply(Encode(Error("invalid staged authorization")));return android::binder::Status::ok();
                }
                ref=request["ref"].asString();remote=request["remote"].asString();uid=request["compositorUid"].asInt();
            }
            /* Bounded heap storage; the inherited channel frame limit applies. */
            std::unique_ptr<FlatpakStagedManifest> manifest;
            if(verify) {
                manifest=std::make_unique<FlatpakStagedManifest>();
                if(!StagedManifest(request["manifest"],manifest.get()) || request["operationId"].asString()!=manifest->operation_id) {
                    reply(Encode(Error("invalid staged manifest")));return android::binder::Status::ok();
                }
            }
            std::unique_lock<std::mutex> lifecycle(g_identity_lifecycle_mutex,std::try_to_lock);
            if(!lifecycle.owns_lock()){reply(Encode(Error("another handoff is active")));return android::binder::Status::ok();}
            {std::lock_guard<std::mutex> lock(g_identity_mutex);
             if(!g_identity){reply(Encode(Error("bridge identity provider unavailable")));return android::binder::Status::ok();}}
            FlatpakResult result{};
            if(verify) {
                g_staging_operation=request["operationId"].asString();g_last_staging_progress.clear();
                flatpak_publish_progress(HandoffProgress);Snapshot(g_staging_operation,"verifying");
            }
            flatpak_manager_staged(command.c_str(),request["operationId"].asCString(),ref.c_str(),remote.c_str(),uid,manifest.get(),&result);
            if(verify) {
                flatpak_publish_progress(nullptr);
                Snapshot(g_staging_operation,result.ok?"handoff":"complete",result.error?result.error:"");
                g_staging_operation.clear();
            }
            reply(Encode(EncodeResult(result)));flatpak_manager_result_clear(&result);
            return android::binder::Status::ok();
        }
        if (command == "bind_pending" || command == "abort_pending" ||
                command == "pending_metadata" || command == "pending_desktop" || command == "pending_icon" ||
                command == "install_complete" || command == "install_failed") {
            std::unique_lock<std::mutex> lifecycle(g_identity_lifecycle_mutex,std::try_to_lock);
            if(!lifecycle.owns_lock()){reply(Encode(Error("another handoff is active")));return android::binder::Status::ok();}
            {std::lock_guard<std::mutex> lock(g_identity_mutex);
             if(!g_identity){reply(Encode(Error("bridge identity provider unavailable")));return android::binder::Status::ok();}}
            if (!OnlyKeys(request, {"ref", "remote", "operationId", "uid", "packageName", "error"}) ||
                    !request["ref"].isString() || !request["operationId"].isString() ||
                    !ValidName(request["operationId"].asString(), 64) ||
                    HasEmbeddedNul(request["ref"].asString()) ||
                    (request.isMember("packageName") && (!request["packageName"].isString() || HasEmbeddedNul(request["packageName"].asString()))) ||
                    (request.isMember("error") && (!request["error"].isString() || HasEmbeddedNul(request["error"].asString()))) ||
                    (request.isMember("remote") && (!request["remote"].isString() || HasEmbeddedNul(request["remote"].asString())))) {
                reply(Encode(Error("invalid deployment handoff")));return android::binder::Status::ok();
            }
            const std::string ref=request["ref"].asString(), operation=request["operationId"].asString();
            const std::string remote=request.get("remote","flathub").asString();
            if(request.isMember("uid") && !request["uid"].isInt()) {
                reply(Encode(Error("invalid stub UID")));return android::binder::Status::ok();
            }
            int uid=request.get("uid",-1).asInt();
            if(command == "bind_pending") {
                char verified_ref[1100], verified_remote[130], error[512];
                if(!request["uid"].isInt() || !request["packageName"].isString() ||
                        stub_verify_caller(uid,request["packageName"].asCString(),verified_ref,sizeof(verified_ref),
                            verified_remote,sizeof(verified_remote),error,sizeof(error)) || ref != verified_ref || remote != verified_remote) {
                    reply(Encode(Error("unverified final stub")));return android::binder::Status::ok();
                }
            }
            if(command == "install_complete" || command == "install_failed") {
                const bool ok=command == "install_complete";
                if(ok && flatpak_manager_complete_handoff(operation.c_str(),ref.c_str(),remote.c_str())) {
                    reply(Encode(Error("operation has not completed binding")));return android::binder::Status::ok();
                }
                const std::string error=ok ? "" : request.get("error","Install failed").asString();
                Snapshot(operation,"complete",error);
                Json::Value event;event["phase"]="complete";
                event["result"]["ok"]=ok;event["result"]["operationId"]=operation;event["result"]["ref"]=ref;
                if(!ok)event["result"]["error"]=error;
                NotifyListeners(Encode(event));
                Json::Value out;out["ok"]=true;reply(Encode(out));return android::binder::Status::ok();
            }

            FlatpakResult result{};
            flatpak_manager_handoff(command.c_str(),ref.c_str(),remote.c_str(),operation.c_str(),uid,&result);

            reply(Encode(EncodeResult(result)));flatpak_manager_result_clear(&result);
            return android::binder::Status::ok();
        }
        if(command == "reconcile_deployments") {
            if(!OnlyKeys(request,{}))reply(Encode(Error("unexpected arguments")));
            else { flatpak_manager_reconcile_runtime_refs();Json::Value out;out["ok"]=true;reply(Encode(out)); }
            return android::binder::Status::ok();
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
        if(command=="stage" || command=="install" || command=="deploy_pending") {
            reply(Encode(Error("Compositor installer required; legacy install disabled")));
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
        int app_uid = -1;
        std::string operation_id_storage;
        const char* operation_id = nullptr;
        if (command == "install" || command == "uninstall" || command == "installed_for_uid" || command == "check_update" || command == "installed_commit" || (command == "metadata" || command == "desktop_entry" || command == "icon")) {
            const bool uninstall = command == "uninstall";
            const bool install = command == "install";
            const bool installed_for_uid = command == "installed_for_uid" || command == "check_update" || command == "installed_commit";
            if (!(uninstall ? OnlyKeys(request, {"ref", "uid", "deleteData", "operationId"}) :
                    install ? OnlyKeys(request, {"ref", "operationId", "appCommit", "runtimeRef", "runtimeCommit", "remote", "uid"}) :
                    installed_for_uid ? OnlyKeys(request, {"ref", "uid"}) :
                    OnlyKeys(request, {"ref", "uid", "operationId"})) || !request["ref"].isString() ||
                    (install && (!request["appCommit"].isString() || !request["runtimeRef"].isString() ||
                        !request["runtimeCommit"].isString() || !request["remote"].isString() ||
                        !request["uid"].isInt())) ||
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
                app_uid = request["uid"].asInt();
                if (app_uid < 10000) {
                    reply(Encode(Error("valid app UID required")));
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
        } else if (command == "install_repo" || command == "remove_repo") {
            if(!OnlyKeys(request,command=="install_repo" ? std::initializer_list<const char*>{"remote","file"} : std::initializer_list<const char*>{"remote"}) ||
                    !request["remote"].isString() || HasEmbeddedNul(request["remote"].asString()) ||
                    (command=="install_repo" && (!request["file"].isString() || request["file"].asString().size()>65536 || HasEmbeddedNul(request["file"].asString())))) {
                reply(Encode(Error("Invalid repository request")));return android::binder::Status::ok();
            }
            remote_storage=request["remote"].asString();remote=remote_storage.c_str();
            if(command=="install_repo"){ref_storage=request["file"].asString();ref=ref_storage.c_str();}
        } else if (command == "list_remotes" || command == "add_flathub") {
            if(!OnlyKeys(request,{})) {
                reply(Encode(Error("unexpected arguments")));return android::binder::Status::ok();
            }
        } else if (!OnlyKeys(request, {})) {
            reply(Encode(Error("unexpected arguments")));
            return android::binder::Status::ok();
        }

        if (command == "install" || command == "uninstall") PublishProgress(command + " started");
        FlatpakResult result{};
        flatpak_manager_call(command.c_str(), ref, delete_data,
                operation_id, app_commit, runtime_ref, runtime_commit, remote, app_uid, &result);
        reply(Encode(EncodeResult(result)));
        flatpak_manager_result_clear(&result);
        return android::binder::Status::ok();
    }

    android::binder::Status subscribe(const android::String16& topic16,
            const android::sp<ILinuxdListener>& listener) override {
        if (!IsSubsystemCaller()) return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY);
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
