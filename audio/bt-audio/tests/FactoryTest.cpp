// Copyright 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "../BluetoothAudioProviderFactory.h"

using namespace aidl::android::hardware::bluetooth::audio;

TEST(SoftwareFactory, AdvertisedSessionsHaveProvidersAndPcmOnly) {
    auto factory = ndk::SharedRefBase::make<BluetoothAudioProviderFactory>();
    for (auto type : {
                 SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH,
                 SessionType::A2DP_SOFTWARE_DECODING_DATAPATH,
                 SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH,
                 SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH,
                 SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH,
                 SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH,
                 SessionType::LE_AUDIO_BROADCAST_SOFTWARE_DECODING_DATAPATH,
                 SessionType::HFP_SOFTWARE_ENCODING_DATAPATH,
                 SessionType::HFP_SOFTWARE_DECODING_DATAPATH,
         }) {
        SCOPED_TRACE(toString(type));
        std::vector<AudioCapabilities> caps;
        ASSERT_TRUE(factory->getProviderCapabilities(type, &caps).isOk());
        ASSERT_FALSE(caps.empty());
        for (const auto& cap : caps) EXPECT_EQ(AudioCapabilities::pcmCapabilities, cap.getTag());
        std::shared_ptr<IBluetoothAudioProvider> provider;
        ASSERT_TRUE(factory->openProvider(type, &provider).isOk());
        ASSERT_NE(nullptr, provider);
        std::optional<IBluetoothAudioProviderFactory::ProviderInfo> info;
        EXPECT_FALSE(factory->getProviderInfo(type, &info).isOk());
        EXPECT_FALSE(info.has_value());
    }
}

TEST(SoftwareFactory, RejectsAllOtherSessionsAndClearsReusedResults) {
    auto factory = ndk::SharedRefBase::make<BluetoothAudioProviderFactory>();
    auto types = ndk::enum_range<SessionType>();
    for (auto type : types) {
        // The capability query is also the supported-session contract.
        std::vector<AudioCapabilities> caps;
        ASSERT_TRUE(factory->getProviderCapabilities(type, &caps).isOk());
        if (!caps.empty()) continue;
        std::shared_ptr<IBluetoothAudioProvider> provider;
        ASSERT_TRUE(factory->openProvider(SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH,
                                          &provider).isOk());
        EXPECT_FALSE(factory->openProvider(type, &provider).isOk());
        EXPECT_EQ(nullptr, provider);
        ASSERT_TRUE(factory->getProviderCapabilities(SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH,
                                                     &caps).isOk());
        ASSERT_FALSE(caps.empty());
        EXPECT_TRUE(factory->getProviderCapabilities(type, &caps).isOk());
        EXPECT_TRUE(caps.empty());
    }
    std::vector<AudioCapabilities> caps;
    std::shared_ptr<IBluetoothAudioProvider> provider;
    EXPECT_TRUE(factory->getProviderCapabilities(static_cast<SessionType>(-1), &caps).isOk());
    EXPECT_TRUE(caps.empty());
    EXPECT_FALSE(factory->openProvider(static_cast<SessionType>(-1), &provider).isOk());
}
