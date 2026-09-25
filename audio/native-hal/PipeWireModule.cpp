#include "PipeWireModule.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

#include "core-impl/Stream.h"

namespace aidl::android::hardware::audio::core {

namespace {
constexpr char kProxySocket[] = "/data/vendor/maton-audio/audio.sock";
constexpr uint32_t kMagic = 0x4d415544;  // MAUD
constexpr uint32_t kPcmS16Le = 1;

struct Request {
    uint32_t magic;
    uint32_t direction;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t format;
};

bool transferAll(int fd, void* data, size_t bytes, bool writeToFd) {
    auto* cursor = static_cast<uint8_t*>(data);
    while (bytes > 0) {
        ssize_t n = writeToFd ? write(fd, cursor, bytes) : read(fd, cursor, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        cursor += n;
        bytes -= static_cast<size_t>(n);
    }
    return true;
}

class PipeWireDriver : public StreamCommonImpl {
  public:
    PipeWireDriver(StreamContext* context, const Metadata& metadata, bool input)
        : StreamCommonImpl(context, metadata), mInput(input) {}
    ~PipeWireDriver() override { cleanupWorker(); }

    ::android::status_t init(DriverCallbackInterface*) override { return ::android::OK; }
    ::android::status_t drain(StreamDescriptor::DrainMode) override { return ::android::OK; }
    ::android::status_t flush() override { closeSocket(); return ::android::OK; }
    ::android::status_t pause() override { return ::android::OK; }
    ::android::status_t standby() override { closeSocket(); return ::android::OK; }
    ::android::status_t start() override { connectSocket(); return ::android::OK; }

    ::android::status_t transfer(void* buffer, size_t frames, size_t* actualFrames,
                                 int32_t* latencyMs) override {
        const size_t frameBytes = mContext.getFrameSize();
        if (frameBytes == 0 || frameBytes % sizeof(int16_t) != 0 || frameBytes > 4 ||
            mContext.getFormat().encoding != "AUDIO_FORMAT_PCM_16_BIT") {
            return ::android::BAD_VALUE;
        }
        const size_t bytes = frames * frameBytes;
        if (mInput) memset(buffer, 0, bytes);
        if (mSocket < 0) connectSocketIfDue();
        if (mSocket >= 0 && !transferAll(mSocket, buffer, bytes, !mInput)) closeSocket();
        *actualFrames = frames;
        *latencyMs = mContext.getNominalLatencyMs();
        return ::android::OK;
    }

    void shutdown() override { closeSocket(); }

  private:
    void connectSocketIfDue() {
        const auto now = std::chrono::steady_clock::now();
        if (now < mNextConnect) return;
        mNextConnect = now + std::chrono::milliseconds(250);
        connectSocket();
    }

    bool connectSocket() {
        if (mSocket >= 0) return true;
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        // A stalled proxy must not wedge an AudioFlinger stream worker. If
        // PipeWire has no usable device, playback drops and capture is silence.
        const timeval timeout{0, 250000};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            ::close(fd);
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        strlcpy(address.sun_path, kProxySocket, sizeof(address.sun_path));
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd);
            return false;
        }
        Request request{kMagic, mInput ? 1u : 0u,
                        static_cast<uint32_t>(mContext.getSampleRate()),
                        static_cast<uint32_t>(mContext.getFrameSize() / sizeof(int16_t)),
                        kPcmS16Le};
        if (!transferAll(fd, &request, sizeof(request), true)) {
            ::close(fd);
            return false;
        }
        mSocket = fd;
        return true;
    }

    void closeSocket() {
        if (mSocket >= 0) ::close(std::exchange(mSocket, -1));
    }

    const bool mInput;
    int mSocket = -1;
    std::chrono::steady_clock::time_point mNextConnect{};
};

class PipeWireInput final : public StreamIn, public PipeWireDriver {
  public:
    PipeWireInput(StreamContext&& context,
                  const ::aidl::android::hardware::audio::common::SinkMetadata& metadata,
                  const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& mics)
        : StreamIn(std::move(context), mics), PipeWireDriver(&mContextInstance, metadata, true) {}

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

class PipeWireOutput final : public StreamOut, public PipeWireDriver {
  public:
    PipeWireOutput(StreamContext&& context,
                   const ::aidl::android::hardware::audio::common::SourceMetadata& metadata,
                   const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>& offload)
        : StreamOut(std::move(context), offload), PipeWireDriver(&mContextInstance, metadata, false) {}

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};
}  // namespace

ndk::ScopedAStatus PipeWireModule::createInputStream(
        StreamContext&& context,
        const ::aidl::android::hardware::audio::common::SinkMetadata& metadata,
        const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones,
        std::shared_ptr<StreamIn>* result) {
    return createStreamInstance<PipeWireInput>(result, std::move(context), metadata, microphones);
}

ndk::ScopedAStatus PipeWireModule::createOutputStream(
        StreamContext&& context,
        const ::aidl::android::hardware::audio::common::SourceMetadata& metadata,
        const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>& offload,
        std::shared_ptr<StreamOut>* result) {
    return createStreamInstance<PipeWireOutput>(result, std::move(context), metadata, offload);
}

}  // namespace aidl::android::hardware::audio::core
