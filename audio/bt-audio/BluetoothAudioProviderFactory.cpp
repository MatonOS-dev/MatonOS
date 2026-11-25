/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "MatonBtAudioFactory"

#include "BluetoothAudioProviderFactory.h"
#include <BluetoothAudioCodecs.h>
#include <android-base/logging.h>

#include "A2dpSoftwareAudioProvider.h"
#include "BluetoothAudioProvider.h"
#include "HearingAidAudioProvider.h"
#include "HfpSoftwareAudioProvider.h"
#include "LeAudioSoftwareAudioProvider.h"

namespace aidl::android::hardware::bluetooth::audio {
namespace {
bool isSoftwareSession(SessionType type) {
    switch (type) {
        case SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH:
        case SessionType::A2DP_SOFTWARE_DECODING_DATAPATH:
        case SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH:
        case SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH:
        case SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH:
        case SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH:
        case SessionType::LE_AUDIO_BROADCAST_SOFTWARE_DECODING_DATAPATH:
        case SessionType::HFP_SOFTWARE_ENCODING_DATAPATH:
        case SessionType::HFP_SOFTWARE_DECODING_DATAPATH:
            return true;
        default:
            return false;
    }
}
}  // namespace

BluetoothAudioProviderFactory::BluetoothAudioProviderFactory() = default;

ndk::ScopedAStatus BluetoothAudioProviderFactory::openProvider(
        SessionType type, std::shared_ptr<IBluetoothAudioProvider>* result) {
    result->reset();
    std::shared_ptr<BluetoothAudioProvider> provider;
    switch (type) {
        case SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH:
            provider = ndk::SharedRefBase::make<A2dpSoftwareEncodingAudioProvider>(); break;
        case SessionType::A2DP_SOFTWARE_DECODING_DATAPATH:
            provider = ndk::SharedRefBase::make<A2dpSoftwareDecodingAudioProvider>(); break;
        case SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH:
            provider = ndk::SharedRefBase::make<HearingAidAudioProvider>(); break;
        case SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH:
            provider = ndk::SharedRefBase::make<LeAudioSoftwareOutputAudioProvider>(); break;
        case SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH:
            provider = ndk::SharedRefBase::make<LeAudioSoftwareInputAudioProvider>(); break;
        case SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH:
            provider = ndk::SharedRefBase::make<LeAudioSoftwareBroadcastOutputAudioProvider>(); break;
        case SessionType::LE_AUDIO_BROADCAST_SOFTWARE_DECODING_DATAPATH:
            provider = ndk::SharedRefBase::make<LeAudioSoftwareBroadcastInputAudioProvider>(); break;
        case SessionType::HFP_SOFTWARE_ENCODING_DATAPATH:
            provider = ndk::SharedRefBase::make<HfpSoftwareOutputAudioProvider>(); break;
        case SessionType::HFP_SOFTWARE_DECODING_DATAPATH:
            provider = ndk::SharedRefBase::make<HfpSoftwareInputAudioProvider>(); break;
        default:
            LOG(ERROR) << __func__ << ": unsupported session=" << toString(type);
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (!provider->isValid(type)) {
        LOG(ERROR) << __func__ << ": invalid provider for session=" << toString(type);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    LOG(INFO) << __func__ << ": software session=" << toString(type);
    *result = std::move(provider);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioProviderFactory::getProviderCapabilities(
        SessionType type, std::vector<AudioCapabilities>* result) {
    result->clear();
    // Unsupported/offload sessions must have no capabilities. In particular,
    // never fall through to software PCM capabilities for an unknown enum.
    if (isSoftwareSession(type)) {
        for (const auto& pcm : BluetoothAudioCodecs::GetSoftwarePcmCapabilities()) {
            AudioCapabilities capability;
            capability.set<AudioCapabilities::pcmCapabilities>(pcm);
            result->push_back(std::move(capability));
        }
    }
    LOG(INFO) << __func__ << ": session=" << toString(type) << " capabilities=" << result->size();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothAudioProviderFactory::getProviderInfo(
        SessionType type, std::optional<ProviderInfo>* result) {
    *result = std::nullopt;
    LOG(INFO) << __func__ << ": no hardware codec provider for session=" << toString(type);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
}  // namespace aidl::android::hardware::bluetooth::audio
