#define LOG_TAG "AHAL_MainPipeWire"

#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <Log.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <pipewire/pipewire.h>

#include "ModulePipeWire.h"
#include "core-impl/AudioPolicyConfigXmlConverter.h"
#include "core-impl/ChildInterface.h"
#include "core-impl/Config.h"
#include "core-impl/Module.h"

using aidl::android::hardware::audio::core::ChildInterface;
using aidl::android::hardware::audio::core::Config;
using aidl::android::hardware::audio::core::Module;
using aidl::android::hardware::audio::core::ModulePipeWire;
using aidl::android::hardware::audio::core::internal::AudioPolicyConfigXmlConverter;

namespace {
bool addService(const ndk::SpAIBinder& binder, const std::string& name) {
    const binder_status_t status = AServiceManager_addService(binder.get(), name.c_str());
    if (status == STATUS_OK) return true;
    LOG(ERROR) << __func__ << ": failed to register service=" << name << " status=" << status;
    return false;
}

ChildInterface<Module> createModule(const std::string& name,
                                    std::unique_ptr<Module::Configuration>&& config) {
    const auto moduleType = Module::typeFromString(name);
    if (!moduleType.has_value()) {
        LOG(ERROR) << __func__ << ": unsupported configured audio module=" << name;
        return {};
    }
    std::shared_ptr<Module> module;
    if (name == "default") {
        module = ndk::SharedRefBase::make<ModulePipeWire>(std::move(config));
    } else {
        // Preserve the AOSP example implementation for Bluetooth, USB,
        // remote-submix, and stub modules configured beside the primary.
        module = Module::createInstance(*moduleType, std::move(config));
    }
    if (module == nullptr) {
        LOG(ERROR) << __func__ << ": failed to create configured module=" << name;
        return {};
    }
    const std::string serviceName = std::string(Module::descriptor) + "/" + name;
    if (!addService(module->asBinder(), serviceName)) return {};
    ChildInterface<Module> result;
    result = std::move(module);
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    std::srand(std::time(nullptr));
    pw_init(&argc, &argv);
    ABinderProcess_setThreadPoolMaxThreadCount(16);
    ABinderProcess_startThreadPool();
    LOG(INFO) << "Starting MatonOS PipeWire AIDL audio HAL";

    AudioPolicyConfigXmlConverter converter{::android::audio_get_audio_policy_config_file()};
    auto config = ndk::SharedRefBase::make<Config>(converter);
    if (!addService(config->asBinder(), std::string(Config::descriptor) + "/default")) {
        return EXIT_FAILURE;
    }

    std::vector<ChildInterface<Module>> modules;
    auto configuredModules = converter.releaseModuleConfigs();
    for (auto& [name, moduleConfig] : *configuredModules) {
        auto module = createModule(name, std::move(moduleConfig));
        if (!module) return EXIT_FAILURE;
        modules.push_back(std::move(module));
    }
    ABinderProcess_joinThreadPool();
    pw_deinit();
    return EXIT_FAILURE;
}
