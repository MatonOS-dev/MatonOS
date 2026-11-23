#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <atomic>
#include <mutex>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>

#include "core-impl/Stream.h"

namespace aidl::android::hardware::audio::core {

class StreamPipeWire : public StreamCommonImpl {
  public:
    StreamPipeWire(StreamContext* context, const Metadata& metadata, bool input);
    ~StreamPipeWire() override;

    ::android::status_t init(DriverCallbackInterface* callback) override;
    ::android::status_t drain(StreamDescriptor::DrainMode mode) override;
    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;
    ::android::status_t refinePosition(StreamDescriptor::Position* position) override;
    void shutdown() override;

  private:
    static void onStateChanged(void* userData, pw_stream_state oldState,
                               pw_stream_state state, const char* error);
    static void onProcess(void* userData);
    static void onDrained(void* userData);
    ::android::status_t startPipeWire();
    void stopPipeWire();
    void processBuffer();

    static constexpr size_t kMaxQueuedBytes = 2 * 1024 * 1024;
    static constexpr int kOperationTimeoutMs = 2000;

    const bool mInput;
    const uint32_t mSampleRate;
    const uint32_t mChannels;
    const uint32_t mBytesPerSample;
    const uint32_t mFrameBytes;
    const uint32_t mSpaFormat;
    pw_thread_loop* mLoop = nullptr;
    pw_context* mPwContext = nullptr;
    pw_core* mCore = nullptr;
    pw_stream* mStream = nullptr;
    std::atomic<pw_stream_state> mState{PW_STREAM_STATE_UNCONNECTED};
    spa_hook mStreamListener{};
    std::mutex mBufferMutex;
    std::condition_variable mBufferChanged;
    std::deque<uint8_t> mPlaybackQueue;
    std::deque<uint8_t> mCaptureQueue;
    int64_t mFramesTransferred = 0;
    bool mInitialized = false;
    bool mShutdown = false;
    bool mDrained = false;
    bool mLoopStarted = false;
};

class StreamInPipeWire final : public StreamIn, public StreamPipeWire {
  public:
    friend class ndk::SharedRefBase;
    StreamInPipeWire(StreamContext&& context,
                     const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
                     const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>&
                             microphones);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

class StreamOutPipeWire final : public StreamOut, public StreamPipeWire {
  public:
    friend class ndk::SharedRefBase;
    StreamOutPipeWire(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                    offloadInfo);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

}  // namespace aidl::android::hardware::audio::core
