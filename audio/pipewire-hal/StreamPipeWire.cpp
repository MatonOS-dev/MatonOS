#define LOG_TAG "AHAL_StreamPipeWire"

#include "StreamPipeWire.h"
#include "AudioControlChannel.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cmath>
#include <limits>
#include <string>
#include <thread>

#include <Log.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

namespace aidl::android::hardware::audio::core {
namespace {
constexpr char kPlaybackNodeName[] = "Android";
constexpr char kCaptureNodeName[] = "Android mic";

bool isTerminalState(pw_stream_state state) {
    return state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED;
}
}  // namespace

StreamPipeWire::StreamPipeWire(StreamContext* context, const Metadata& metadata, bool input)
    : StreamCommonImpl(context, metadata),
      mInput(input),
      mSampleRate(context->getSampleRate() > 0 ? context->getSampleRate() : 48000),
      mChannels(context->getFrameSize() /
                (context->getFormat().pcm == ::aidl::android::media::audio::common::PcmType::FLOAT_32_BIT
                         ? sizeof(float)
                         : context->getFormat().pcm ==
                                           ::aidl::android::media::audio::common::PcmType::INT_16_BIT
                                   ? sizeof(int16_t)
                                   : 1)),
      mBytesPerSample(context->getFormat().pcm ==
                                      ::aidl::android::media::audio::common::PcmType::FLOAT_32_BIT
                              ? sizeof(float)
                              : context->getFormat().pcm ==
                                                ::aidl::android::media::audio::common::PcmType::INT_16_BIT
                                        ? sizeof(int16_t)
                                        : 0),
      mFrameBytes(context->getFrameSize()),
      mSpaFormat(context->getFormat().pcm ==
                                 ::aidl::android::media::audio::common::PcmType::FLOAT_32_BIT
                         ? SPA_AUDIO_FORMAT_F32_LE
                         : SPA_AUDIO_FORMAT_S16_LE) {}

StreamPipeWire::~StreamPipeWire() {
    cleanupWorker();
    shutdown();
}

::android::status_t StreamPipeWire::init(DriverCallbackInterface*) {
    if (mFrameBytes == 0 || mBytesPerSample == 0 || mFrameBytes % mBytesPerSample != 0 ||
        mChannels == 0 ||
        (mSpaFormat != SPA_AUDIO_FORMAT_S16_LE && mSpaFormat != SPA_AUDIO_FORMAT_F32_LE)) {
        LOG(ERROR) << __func__ << ": unsupported PCM frame size=" << mFrameBytes
                   << " channels=" << mChannels;
        return ::android::BAD_VALUE;
    }
    mInitialized = true;
    return ::android::OK;
}

::android::status_t StreamPipeWire::start() {
    if (!mInitialized || mShutdown) {
        LOG(ERROR) << __func__ << ": stream cannot start initialized=" << mInitialized
                   << " shutdown=" << mShutdown;
        return ::android::INVALID_OPERATION;
    }
    if (mState == PW_STREAM_STATE_ERROR) {
        mPipeWireDown = true;
    }
    if (mPipeWireDown) {
        if (mLoop != nullptr || mStream != nullptr || mPwContext != nullptr || mCore != nullptr) {
            stopPipeWire();
        }
        return ::android::OK;
    }
    if (mStream != nullptr) {
        if (pw_stream_set_active(mStream, true) < 0) {
            LOG(ERROR) << __func__ << ": failed to reactivate PipeWire stream";
            return ::android::INVALID_OPERATION;
        }
        return ::android::OK;
    }
    const auto status = startPipeWire();
    if (status != ::android::OK) {
        mPipeWireDown = true;
        mNextRetryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count() +
                       1000000000LL;
        // Keep the AIDL stream alive; transfer() supplies paced silence/discard until recovery.
        return ::android::OK;
    }
    return status;
}

::android::status_t StreamPipeWire::startPipeWire() {
    const char* nodeName = mInput ? kCaptureNodeName : kPlaybackNodeName;
    const char* mediaClass = mInput ? "Stream/Input/Audio" : "Stream/Output/Audio";
    mLoop = pw_thread_loop_new("matonos-audio", nullptr);
    if (mLoop == nullptr) {
        PLOG(ERROR) << __func__ << ": pw_thread_loop_new failed for " << nodeName;
        return ::android::NO_INIT;
    }
    if (int result = pw_thread_loop_start(mLoop); result < 0) {
        LOG(ERROR) << __func__ << ": pw_thread_loop_start failed node=" << nodeName
                   << " result=" << result;
        stopPipeWire();
        return ::android::NO_INIT;
    }
    mLoopStarted = true;

    pw_thread_loop_lock(mLoop);
    mPwContext = pw_context_new(pw_thread_loop_get_loop(mLoop), nullptr, 0);
    if (mPwContext == nullptr) {
        LOG(ERROR) << __func__ << ": pw_context_new failed node=" << nodeName;
        pw_thread_loop_unlock(mLoop);
        stopPipeWire();
        return ::android::NO_INIT;
    }
    mCore = pw_context_connect(mPwContext, nullptr, 0);
    if (mCore == nullptr) {
        PLOG(ERROR) << __func__ << ": PipeWire server unavailable for node=" << nodeName;
        pw_thread_loop_unlock(mLoop);
        stopPipeWire();
        return ::android::NO_INIT;
    }
    static const pw_core_events coreEvents = [] {
        pw_core_events value{};
        value.version = PW_VERSION_CORE_EVENTS;
        value.error = &StreamPipeWire::onCoreError;
        return value;
    }();
    pw_core_add_listener(mCore, &mCoreListener, &coreEvents, this);
    mCoreListenerInstalled = true;

    const auto& devices = getConnectedDevices();
    if (!mInput && !devices.empty()) mActiveIdentity = pipeWireIdentity(devices.front().address);
    const uint32_t targetNode = getPipeWireTargetNode(mActiveIdentity);
    pw_properties* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, mInput ? "Capture" : "Playback",
            PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, nodeName, PW_KEY_NODE_DESCRIPTION,
            nodeName, PW_KEY_MEDIA_CLASS, mediaClass, nullptr);
    if (properties == nullptr) {
        LOG(ERROR) << __func__ << ": pw_properties_new failed node=" << nodeName;
        pw_thread_loop_unlock(mLoop);
        stopPipeWire();
        return ::android::NO_MEMORY;
    }
    mStream = pw_stream_new(mCore, nodeName, properties);
    if (mStream == nullptr) {
        LOG(ERROR) << __func__ << ": pw_stream_new failed node=" << nodeName;
        pw_thread_loop_unlock(mLoop);
        stopPipeWire();
        return ::android::NO_INIT;
    }

    static const pw_stream_events events = [] {
        pw_stream_events value{};
        value.version = PW_VERSION_STREAM_EVENTS;
        value.state_changed = &StreamPipeWire::onStateChanged;
        value.process = &StreamPipeWire::onProcess;
        value.drained = &StreamPipeWire::onDrained;
        return value;
    }();
    pw_stream_add_listener(mStream, &mStreamListener, &events, this);

    const ::android::status_t connectStatus = connectPipeWireNode(targetNode);
    if (connectStatus != ::android::OK) {
        pw_thread_loop_unlock(mLoop);
        stopPipeWire();
        return connectStatus;
    }
    if (!mInput) {
        const float boost = getPipeWireDeviceBoost(mActiveIdentity);
        float volume = boost;
        const int volumeResult = pw_stream_set_control(mStream, SPA_PROP_volume, 1, &volume);
        if (volumeResult < 0) {
            LOG(ERROR) << __func__ << ": pw_stream_set_control(volume) failed identity="
                       << mActiveIdentity << " multiplier=" << boost << " result=" << volumeResult
                       << "; using PCM gain fallback";
            mUsePcmBoost = true;
        } else {
            mAppliedBoost = boost;
            mUsePcmBoost = false;
        }
    }
    pw_thread_loop_unlock(mLoop);
    return ::android::OK;
}

::android::status_t StreamPipeWire::connectPipeWireNode(uint32_t targetNode) {
    uint8_t podBuffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    const spa_pod* params[] = {static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_AUDIO_format, SPA_POD_Id(mSpaFormat),
            SPA_FORMAT_AUDIO_rate, SPA_POD_Int(mSampleRate),
            SPA_FORMAT_AUDIO_channels, SPA_POD_Int(mChannels)))};
    mState = PW_STREAM_STATE_UNCONNECTED;
    const int result = pw_stream_connect(
            mStream, mInput ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, targetNode,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1);
    if (result < 0) {
        LOG(ERROR) << __func__ << ": pw_stream_connect failed input=" << mInput
                   << " target=" << targetNode << " result=" << result;
        return ::android::NO_INIT;
    }
    int waitResult = 0;
    while (mState == PW_STREAM_STATE_UNCONNECTED || mState == PW_STREAM_STATE_CONNECTING) {
        waitResult = pw_thread_loop_timed_wait(mLoop, kOperationTimeoutMs / 1000);
        if (waitResult < 0 || isTerminalState(mState)) break;
    }
    if (mState != PW_STREAM_STATE_PAUSED && mState != PW_STREAM_STATE_STREAMING) {
        LOG(ERROR) << __func__ << ": PipeWire stream did not become ready input=" << mInput
                   << " target=" << targetNode << " state=" << pw_stream_state_as_string(mState)
                   << " wait_result=" << waitResult;
        return ::android::NO_INIT;
    }
    return ::android::OK;
}

void StreamPipeWire::onStateChanged(void* userData, pw_stream_state /*oldState*/,
                                    pw_stream_state state,
                                    const char* error) {
    auto* self = static_cast<StreamPipeWire*>(userData);
    self->mState = state;
    if (state == PW_STREAM_STATE_ERROR) {
        const int64_t retryAt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch()).count() +
                                1000000000LL;
        self->mNextRetryNs = retryAt;
        if (!self->mPipeWireDown.exchange(true)) {
            LOG(ERROR) << __func__ << ": PipeWire stream disconnected node="
                       << (self->mInput ? kCaptureNodeName : kPlaybackNodeName)
                       << " detail=" << (error == nullptr ? "unknown" : error);
        }
    }
    if (self->mLoop != nullptr) pw_thread_loop_signal(self->mLoop, false);
    self->mBufferChanged.notify_all();
}

void StreamPipeWire::onCoreError(void* userData, uint32_t id, int, int result,
                                  const char* message) {
    auto* self = static_cast<StreamPipeWire*>(userData);
    if (id != PW_ID_CORE || result != -EPIPE) {
        LOG(ERROR) << __func__ << ": PipeWire core error id=" << id << " result=" << result
                   << " node=" << (self->mInput ? kCaptureNodeName : kPlaybackNodeName)
                   << " detail=" << (message == nullptr ? "unknown" : message);
        return;
    }
    self->mState = PW_STREAM_STATE_ERROR;
    self->mNextRetryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count() +
                         1000000000LL;
    if (!self->mPipeWireDown.exchange(true)) {
        LOG(ERROR) << __func__ << ": PipeWire core disconnected node="
                   << (self->mInput ? kCaptureNodeName : kPlaybackNodeName)
                   << " result=" << result
                   << " detail=" << (message == nullptr ? "unknown" : message);
    }
    if (self->mLoop != nullptr) pw_thread_loop_signal(self->mLoop, false);
    self->mBufferChanged.notify_all();
}

void StreamPipeWire::onProcess(void* userData) {
    static_cast<StreamPipeWire*>(userData)->processBuffer();
}

void StreamPipeWire::onDrained(void* userData) {
    auto* self = static_cast<StreamPipeWire*>(userData);
    self->mDrained = true;
    if (self->mLoop != nullptr) pw_thread_loop_signal(self->mLoop, false);
}

void StreamPipeWire::processBuffer() {
    pw_buffer* pwBuffer = pw_stream_dequeue_buffer(mStream);
    if (pwBuffer == nullptr) {
        LOG(ERROR) << __func__ << ": pw_stream_dequeue_buffer returned null node="
                   << (mInput ? kCaptureNodeName : kPlaybackNodeName);
        return;
    }
    spa_buffer* buffer = pwBuffer->buffer;
    if (buffer == nullptr || buffer->n_datas == 0 || buffer->datas[0].data == nullptr ||
        buffer->datas[0].chunk == nullptr) {
        LOG(ERROR) << __func__ << ": PipeWire supplied an invalid audio buffer node="
                   << (mInput ? kCaptureNodeName : kPlaybackNodeName);
        pw_stream_queue_buffer(mStream, pwBuffer);
        return;
    }

    spa_data& data = buffer->datas[0];
    spa_chunk& chunk = *data.chunk;
    const size_t capacity = data.maxsize;
    size_t transferred = 0;
    {
        std::lock_guard lock(mBufferMutex);
        if (mInput) {
            size_t bytes = std::min(static_cast<size_t>(chunk.size), capacity);
            bytes -= bytes % mFrameBytes;
            if (mCaptureQueue.size() + bytes > kMaxQueuedBytes) {
                const size_t discard = mCaptureQueue.size() + bytes - kMaxQueuedBytes;
                for (size_t i = 0; i < discard && !mCaptureQueue.empty(); ++i) {
                    mCaptureQueue.pop_front();
                }
                LOG(ERROR) << __func__ << ": capture queue overflow; dropped=" << discard
                           << " bytes";
            }
            const auto* src = static_cast<const uint8_t*>(data.data) + chunk.offset;
            mCaptureQueue.insert(mCaptureQueue.end(), src, src + bytes);
            transferred = bytes;
            chunk.offset = 0;
            chunk.size = bytes;
            chunk.stride = mFrameBytes;
        } else {
            transferred = std::min(capacity, mPlaybackQueue.size());
            transferred -= transferred % mFrameBytes;
            auto* dst = static_cast<uint8_t*>(data.data);
            for (size_t i = 0; i < transferred; ++i) {
                dst[i] = mPlaybackQueue.front();
                mPlaybackQueue.pop_front();
            }
            if (transferred < capacity) memset(dst + transferred, 0, capacity - transferred);
            const float boost = getPipeWireDeviceBoost(mActiveIdentity);
            if (!mUsePcmBoost && boost != mAppliedBoost) {
                float volume = boost;
                const int result = pw_stream_set_control(mStream, SPA_PROP_volume, 1, &volume);
                if (result < 0) {
                    LOG(ERROR) << __func__ << ": pw_stream_set_control(volume) failed identity="
                               << mActiveIdentity << " multiplier=" << boost << " result=" << result
                               << "; using PCM gain fallback";
                    mUsePcmBoost = true;
                } else {
                    mAppliedBoost = boost;
                }
            }
            if (mUsePcmBoost && boost != 1.0f) {
                if (mSpaFormat == SPA_AUDIO_FORMAT_F32_LE) {
                    for (size_t i = 0; i < transferred; i += sizeof(float)) {
                        float sample;
                        memcpy(&sample, dst + i, sizeof(sample));
                        sample *= boost;
                        memcpy(dst + i, &sample, sizeof(sample));
                    }
                } else {
                    for (size_t i = 0; i < transferred; i += sizeof(int16_t)) {
                        int16_t sample;
                        memcpy(&sample, dst + i, sizeof(sample));
                        const int32_t scaled = static_cast<int32_t>(std::lround(sample * boost));
                        const int16_t output = static_cast<int16_t>(std::clamp(
                                scaled, static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
                                static_cast<int32_t>(std::numeric_limits<int16_t>::max())));
                        memcpy(dst + i, &output, sizeof(output));
                    }
                }
            }
            chunk.offset = 0;
            chunk.size = capacity;
            chunk.stride = mFrameBytes;
            mFramesTransferred += static_cast<int64_t>(transferred / mFrameBytes);
        }
    }
    if (pw_stream_queue_buffer(mStream, pwBuffer) < 0) {
        LOG(ERROR) << __func__ << ": pw_stream_queue_buffer failed node="
                   << (mInput ? kCaptureNodeName : kPlaybackNodeName);
    }
    if (transferred != 0) mBufferChanged.notify_all();
    if (!mInput) mBufferChanged.notify_all();
}

::android::status_t StreamPipeWire::transfer(void* buffer, size_t frameCount,
                                              size_t* actualFrameCount, int32_t* latencyMs) {
    *actualFrameCount = 0;
    *latencyMs = getContext().getNominalLatencyMs();
    if (mFrameBytes == 0) return ::android::BAD_VALUE;
    if (mStream == nullptr || mState == PW_STREAM_STATE_ERROR || mPipeWireDown) {
        mPipeWireDown = true;
        return transferWithoutPipeWire(buffer, frameCount, actualFrameCount, latencyMs);
    }
    const size_t byteCount = frameCount * mFrameBytes;
    std::unique_lock lock(mBufferMutex);
    if (mInput) {
        const bool ready = mBufferChanged.wait_for(
                lock, std::chrono::milliseconds(kOperationTimeoutMs),
                [&] { return mCaptureQueue.size() >= byteCount || mState == PW_STREAM_STATE_ERROR; });
        if (!ready || mState == PW_STREAM_STATE_ERROR) {
            lock.unlock();
            if (!mPipeWireDown.exchange(true)) {
                LOG(ERROR) << __func__ << ": capture transfer timed out frames=" << frameCount
                           << " node=" << kCaptureNodeName;
            }
            mNextRetryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count() +
                           1000000000LL;
            return transferWithoutPipeWire(buffer, frameCount, actualFrameCount, latencyMs);
        }
        auto* dst = static_cast<uint8_t*>(buffer);
        for (size_t i = 0; i < byteCount; ++i) {
            dst[i] = mCaptureQueue.front();
            mCaptureQueue.pop_front();
        }
    } else {
        if (mPlaybackQueue.size() + byteCount > kMaxQueuedBytes) {
            LOG(ERROR) << __func__ << ": playback queue limit exceeded frames=" << frameCount;
            return ::android::NO_MEMORY;
        }
        for (size_t i = 0; i < byteCount; ++i) {
            mPlaybackQueue.push_back(static_cast<const uint8_t*>(buffer)[i]);
        }
        const int64_t expectedFrames = mFramesTransferred + static_cast<int64_t>(frameCount);
        const bool complete = mBufferChanged.wait_for(
                lock, std::chrono::milliseconds(kOperationTimeoutMs),
                [&] { return mFramesTransferred >= expectedFrames || mState == PW_STREAM_STATE_ERROR; });
        if (!complete || mState == PW_STREAM_STATE_ERROR) {
            lock.unlock();
            if (!mPipeWireDown.exchange(true)) {
                LOG(ERROR) << __func__ << ": playback transfer timed out frames=" << frameCount
                           << " node=" << kPlaybackNodeName;
            }
            mNextRetryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count() +
                           1000000000LL;
            return transferWithoutPipeWire(buffer, frameCount, actualFrameCount, latencyMs);
        }
    }
    *actualFrameCount = frameCount;
    lock.unlock();
    return ::android::OK;
}

::android::status_t StreamPipeWire::transferWithoutPipeWire(
        void* buffer, size_t frameCount, size_t* actualFrameCount, int32_t* latencyMs) {
    if (mInput && frameCount != 0) memset(buffer, 0, frameCount * mFrameBytes);
    const auto duration = std::chrono::duration<double>(
            static_cast<double>(frameCount) / static_cast<double>(mSampleRate));
    std::this_thread::sleep_for(duration);
    *actualFrameCount = frameCount;
    *latencyMs = getContext().getNominalLatencyMs();
    reconnectPipeWireIfDue();
    return ::android::OK;
}

bool StreamPipeWire::reconnectPipeWireIfDue() {
    if (!mPipeWireDown || mShutdown) return false;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < mNextRetryNs) return false;
    if (mLoop != nullptr || mStream != nullptr || mPwContext != nullptr || mCore != nullptr) {
        stopPipeWire();
    }
    const auto status = startPipeWire();
    if (status != ::android::OK || mState == PW_STREAM_STATE_ERROR) {
        if (status == ::android::OK) stopPipeWire();
        mNextRetryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count() +
                       1000000000LL;
        return false;
    }
    mPipeWireDown = false;
    LOG(INFO) << __func__ << ": PipeWire stream reconnected node="
              << (mInput ? kCaptureNodeName : kPlaybackNodeName);
    return true;
}

::android::status_t StreamPipeWire::drain(StreamDescriptor::DrainMode) {
    if (mInput || mPipeWireDown || mState == PW_STREAM_STATE_ERROR ||
        mStream == nullptr || mLoop == nullptr) return ::android::OK;
    pw_thread_loop_lock(mLoop);
    mDrained = false;
    const int result = pw_stream_flush(mStream, true);
    if (result < 0) {
        LOG(ERROR) << __func__ << ": pw_stream_flush(drain) failed result=" << result;
        pw_thread_loop_unlock(mLoop);
        return ::android::INVALID_OPERATION;
    }
    int waitResult = 0;
    while (!mDrained && mState != PW_STREAM_STATE_ERROR) {
        waitResult = pw_thread_loop_timed_wait(mLoop, kOperationTimeoutMs / 1000);
        if (waitResult < 0) break;
    }
    const bool drained = mDrained;
    pw_thread_loop_unlock(mLoop);
    if (!drained) {
        LOG(ERROR) << __func__ << ": PipeWire drain timed out result=" << waitResult;
        return ::android::INVALID_OPERATION;
    }
    return ::android::OK;
}

::android::status_t StreamPipeWire::flush() {
    if (!mPipeWireDown && mState != PW_STREAM_STATE_ERROR &&
        mStream != nullptr && mLoop != nullptr) {
        pw_thread_loop_lock(mLoop);
        const int pauseResult = pw_stream_set_active(mStream, false);
        const int flushResult = pw_stream_flush(mStream, false);
        const int resumeResult = pw_stream_set_active(mStream, true);
        pw_thread_loop_unlock(mLoop);
        if (pauseResult < 0 || flushResult < 0 || resumeResult < 0) {
            LOG(ERROR) << __func__ << ": PipeWire flush failed pause=" << pauseResult
                       << " flush=" << flushResult << " resume=" << resumeResult;
            return ::android::INVALID_OPERATION;
        }
    }
    {
        std::lock_guard lock(mBufferMutex);
        mPlaybackQueue.clear();
        mCaptureQueue.clear();
        mBufferChanged.notify_all();
    }
    return ::android::OK;
}

::android::status_t StreamPipeWire::pause() {
    if (mPipeWireDown || mState == PW_STREAM_STATE_ERROR || mStream == nullptr) {
        return ::android::OK;
    }
    pw_thread_loop_lock(mLoop);
    const int result = pw_stream_set_active(mStream, false);
    pw_thread_loop_unlock(mLoop);
    if (result < 0) {
        LOG(ERROR) << __func__ << ": pw_stream_set_active(false) failed result=" << result;
        return ::android::INVALID_OPERATION;
    }
    return ::android::OK;
}

::android::status_t StreamPipeWire::standby() {
    stopPipeWire();
    return ::android::OK;
}

ndk::ScopedAStatus StreamPipeWire::setConnectedDevices(
        const std::vector<::aidl::android::media::audio::common::AudioDevice>& devices) {
    auto status = StreamCommonImpl::setConnectedDevices(devices);
    if (!status.isOk()) return status;
    if (mInput || devices.empty()) return status;
    mActiveIdentity = pipeWireIdentity(devices.front().address);
    const uint32_t targetNode = getPipeWireTargetNode(mActiveIdentity);
    if (!mPipeWireDown && mState != PW_STREAM_STATE_ERROR &&
        mStream != nullptr && mLoop != nullptr) {
        pw_thread_loop_lock(mLoop);
        const int disconnectResult = pw_stream_disconnect(mStream);
        if (disconnectResult < 0) {
            LOG(ERROR) << __func__ << ": pw_stream_disconnect failed identity=" << mActiveIdentity
                       << " result=" << disconnectResult;
            pw_thread_loop_unlock(mLoop);
            return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
        }
        const auto connectStatus = connectPipeWireNode(targetNode);
        if (connectStatus != ::android::OK) {
            LOG(ERROR) << __func__ << ": failed to connect selected PipeWire target identity="
                       << mActiveIdentity << " target=" << targetNode << " status=" << connectStatus;
            pw_thread_loop_unlock(mLoop);
            return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
        }
        float volume = getPipeWireDeviceBoost(mActiveIdentity);
        const int volumeResult = pw_stream_set_control(mStream, SPA_PROP_volume, 1, &volume);
        if (volumeResult < 0) {
            LOG(ERROR) << __func__ << ": pw_stream_set_control(volume) failed identity="
                       << mActiveIdentity << " multiplier=" << volume << " result=" << volumeResult
                       << "; using PCM gain fallback";
            mUsePcmBoost = true;
        } else {
            mAppliedBoost = volume;
            mUsePcmBoost = false;
        }
        pw_thread_loop_unlock(mLoop);
    }
    return status;
}

::android::status_t StreamPipeWire::refinePosition(StreamDescriptor::Position* position) {
    if (mPipeWireDown || mState == PW_STREAM_STATE_ERROR ||
        mStream == nullptr || mLoop == nullptr) return ::android::OK;
    pw_time time{};
    pw_thread_loop_lock(mLoop);
    const int result = pw_stream_get_time_n(mStream, &time, sizeof(time));
    pw_thread_loop_unlock(mLoop);
    if (result < 0) {
        LOG(ERROR) << __func__ << ": pw_stream_get_time_n failed result=" << result;
        return ::android::INVALID_OPERATION;
    }
    position->frames = time.ticks > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                               ? std::numeric_limits<int64_t>::max()
                               : static_cast<int64_t>(time.ticks);
    position->timeNs = time.now;
    return ::android::OK;
}

void StreamPipeWire::shutdown() {
    if (mShutdown) return;
    mShutdown = true;
    stopPipeWire();
}

void StreamPipeWire::stopPipeWire() {
    if (mLoop != nullptr && mLoopStarted) {
        pw_thread_loop_lock(mLoop);
        if (mStream != nullptr) {
            spa_hook_remove(&mStreamListener);
            pw_stream_destroy(mStream);
            mStream = nullptr;
        }
        if (mCoreListenerInstalled) {
            spa_hook_remove(&mCoreListener);
            mCoreListenerInstalled = false;
        }
        if (mCore != nullptr) {
            pw_core_disconnect(mCore);
            mCore = nullptr;
        }
        if (mPwContext != nullptr) {
            pw_context_destroy(mPwContext);
            mPwContext = nullptr;
        }
        pw_thread_loop_unlock(mLoop);
    } else if (mLoop != nullptr) {
        if (mStream != nullptr) {
            spa_hook_remove(&mStreamListener);
            pw_stream_destroy(mStream);
            mStream = nullptr;
        }
        if (mCoreListenerInstalled) {
            spa_hook_remove(&mCoreListener);
            mCoreListenerInstalled = false;
        }
        if (mCore != nullptr) {
            pw_core_disconnect(mCore);
            mCore = nullptr;
        }
        if (mPwContext != nullptr) {
            pw_context_destroy(mPwContext);
            mPwContext = nullptr;
        }
    }
    if (mLoop != nullptr) {
        if (mLoopStarted) pw_thread_loop_stop(mLoop);
        pw_thread_loop_destroy(mLoop);
        mLoop = nullptr;
        mLoopStarted = false;
    }
    mState = PW_STREAM_STATE_UNCONNECTED;
    std::lock_guard lock(mBufferMutex);
    mPlaybackQueue.clear();
    mCaptureQueue.clear();
    mBufferChanged.notify_all();
}

StreamInPipeWire::StreamInPipeWire(
        StreamContext&& context, const ::aidl::android::hardware::audio::common::SinkMetadata& metadata,
        const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones)
    : StreamIn(std::move(context), microphones),
      StreamPipeWire(&mContextInstance, metadata, true) {}

StreamOutPipeWire::StreamOutPipeWire(
        StreamContext&& context,
        const ::aidl::android::hardware::audio::common::SourceMetadata& metadata,
        const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamPipeWire(&mContextInstance, metadata, false) {}

}  // namespace aidl::android::hardware::audio::core
