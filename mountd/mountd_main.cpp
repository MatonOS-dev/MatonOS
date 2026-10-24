#include "mountd_core.h"

#include <aidl/vendor/matonos/channel/BnChannel.h>
#include <algorithm>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <json/json.h>
#include <log/log.h>

#include <memory>
#include <string>

using aidl::vendor::matonos::channel::BnChannel;

namespace {
constexpr size_t kMaxJson = 64 * 1024;

class Mountd final : public BnChannel {
  public:
    ndk::ScopedAStatus subscribe(
            const std::string &,
            const std::shared_ptr<aidl::vendor::matonos::channel::IChannelListener> &) override {
        return ndk::ScopedAStatus::fromServiceSpecificError(1);
    }

    ndk::ScopedAStatus unsubscribe(
            const std::string &,
            const std::shared_ptr<aidl::vendor::matonos::channel::IChannelListener> &) override {
        return ndk::ScopedAStatus::fromServiceSpecificError(1);
    }

    ndk::ScopedAStatus call(const std::string &command, const std::string &jsonArgs,
                            std::string *result) override {
        if (!result) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        Json::Value response(Json::objectValue);
        if (jsonArgs.size() > kMaxJson) {
            response["ok"] = false;
            response["fallback"] = true;
            response["error"] = "request_too_large";
        } else if (command == "mount_stub") {
            handleMount(jsonArgs, &response);
        } else if (command == "get_status" && (jsonArgs.empty() || jsonArgs == "{}")) {
            response["ok"] = true;
            response["available"] = true;
            response["mode"] = "extracted_bind";
            response["erofs"] = false;
        } else {
            response["ok"] = false;
            response["fallback"] = true;
            response["error"] = "unsupported_command";
        }
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        *result = Json::writeString(writer, response);
        if (result->size() > kMaxJson) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        return ndk::ScopedAStatus::ok();
    }

  private:
    static void handleMount(const std::string &json, Json::Value *response) {
        Json::CharReaderBuilder builder;
        builder["rejectDupKeys"] = true;
        builder["failIfExtra"] = true;
        Json::Value args;
        std::string errors;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        if (!reader->parse(json.data(), json.data() + json.size(), &args, &errors) ||
            !args.isObject() || args.size() != 6 || !args["pid"].isUInt() ||
            !args["uid"].isUInt() || !args["package"].isString() ||
            !args["deployment"].isString() || !args["signer"].isString() ||
            !args["requestNonce"].isString()) {
            reject(response, "invalid_request");
            return;
        }
        MountdRequest request{};
        request.pid = static_cast<pid_t>(args["pid"].asUInt());
        request.uid = static_cast<uid_t>(args["uid"].asUInt());
        if (!copy(args["package"].asString(), request.package, sizeof(request.package)) ||
            !copy(args["deployment"].asString(), request.deployment, sizeof(request.deployment)) ||
            !copy(args["signer"].asString(), request.signer, sizeof(request.signer)) ||
            !copy(args["requestNonce"].asString(), request.request_nonce, sizeof(request.request_nonce)) ||
            mountd_validate_request(&request)) {
            reject(response, "invalid_identity_or_deployment");
            return;
        }
        (*response)["requestNonce"] = request.request_nonce;
        char error[160] = {};
        if (mountd_mount_request(&request, error, sizeof(error))) {
            ALOGW("Mount request rejected for uid=%u pid=%d: %s", request.uid, request.pid, error);
            reject(response, error[0] ? error : "mount_failed");
            return;
        }
        (*response)["ok"] = true;
        (*response)["fallback"] = false;
        (*response)["mode"] = "mounted";
        (*response)["mounts"] = Json::arrayValue;
        (*response)["mounts"].append("/app");
        (*response)["mounts"].append("/usr");
        (*response)["mounts"].append("/home/" + std::to_string(request.uid));
    }

    static bool copy(const std::string &from, char *to, size_t size) {
        if (from.empty() || from.size() >= size) return false;
        std::copy(from.begin(), from.end(), to);
        to[from.size()] = '\0';
        return true;
    }

    static void reject(Json::Value *response, const char *error) {
        (*response)["ok"] = false;
        (*response)["fallback"] = true;
        (*response)["mode"] = "proxy";
        (*response)["error"] = error;
    }
};
}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto service = ndk::SharedRefBase::make<Mountd>();
    const binder_status_t status = AServiceManager_addService(
        service->asBinder().get(), "vendor.matonos.channel.IChannel/mountd");
    if (status != STATUS_OK) {
        ALOGE("Cannot register mountd channel: %d", status);
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 0;
}
