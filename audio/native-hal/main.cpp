#include <array>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <string>
#include <vector>

#include <Log.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "MatonConfig.h"
#include "PipeWireModule.h"
#include "core-impl/ChildInterface.h"
#include "core-impl/Configuration.h"
#include "core-impl/Module.h"
#include "core-impl/ModuleStub.h"

using aidl::android::hardware::audio::core::ChildInterface;
using aidl::android::hardware::audio::core::MatonConfig;
using aidl::android::hardware::audio::core::Module;
using aidl::android::hardware::audio::core::ModuleStub;
using aidl::android::hardware::audio::core::PipeWireModule;
using aidl::android::hardware::audio::core::internal::getConfiguration;

namespace {
bool addService(const ndk::SpAIBinder& binder, const std::string& name) {
    if (AServiceManager_addService(binder.get(), name.c_str()) == STATUS_OK) return true;
    LOG(ERROR) << "Unable to register audio HAL service " << name;
    return false;
}
}  // namespace

int main() {
    std::srand(std::time(nullptr));
    std::signal(SIGPIPE, SIG_IGN);
    ABinderProcess_setThreadPoolMaxThreadCount(16);
    ABinderProcess_startThreadPool();

    auto config = ndk::SharedRefBase::make<MatonConfig>();
    if (!addService(config->asBinder(), std::string(MatonConfig::descriptor) + "/default")) {
        return EXIT_FAILURE;
    }

    std::vector<ChildInterface<Module>> instances;
    const std::array<std::pair<const char*, Module::Type>, 3> modules{{
            {"default", Module::Type::DEFAULT},
            {"r_submix", Module::Type::R_SUBMIX},
            {"bluetooth", Module::Type::BLUETOOTH},
    }};
    for (const auto& [name, type] : modules) {
        std::shared_ptr<Module> module;
        auto moduleConfig = getConfiguration(type);
        if (type == Module::Type::DEFAULT) {
            module = ndk::SharedRefBase::make<PipeWireModule>(std::move(moduleConfig));
        } else {
            // Keep the policy-required modules registered while leaving their
            // streams safely silent until a dedicated backend is implemented.
            module = ndk::SharedRefBase::make<ModuleStub>(std::move(moduleConfig));
        }
        const std::string instanceName = std::string(Module::descriptor) + "/" + name;
        if (!module || !addService(module->asBinder(), instanceName)) return EXIT_FAILURE;
        instances.emplace_back();
        instances.back() = std::move(module);
    }

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
