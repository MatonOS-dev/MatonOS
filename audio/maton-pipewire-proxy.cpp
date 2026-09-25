// Small PCM bridge between the AIDL audio HAL and PipeWire's Pulse-compatible
// raw stream client. This keeps PipeWire libraries out of the audio HAL APEX.
#include <array>
#include <matonos_ipc.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr char kSocketPath[] = "/data/vendor/maton-audio/audio.sock";
constexpr uint32_t kMagic = 0x4d415544;  // "MAUD"
constexpr uint32_t kPcmS16Le = 1;
constexpr uint32_t kPlayback = 0;
constexpr uint32_t kCapture = 1;
MatonosIpcServer* gIpc = nullptr;

int getState(const char*, char* result, size_t capacity, void*) {
    if (capacity < sizeof("{\"pcmBridge\":true}")) return -1;
    std::strcpy(result, "{\"pcmBridge\":true}");
    return 0;
}

struct Request {
    uint32_t magic;
    uint32_t direction;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
};

bool readAll(int fd, void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size) {
        const ssize_t n = read(fd, p, size);
        if (n == 0) return false;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool copyStream(int from, int to) {
    std::array<uint8_t, 16384> buffer{};
    for (;;) {
        const ssize_t n = read(from, buffer.data(), buffer.size());
        if (n == 0) return true;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        size_t left = static_cast<size_t>(n);
        const uint8_t* p = buffer.data();
        while (left) {
            const ssize_t written = write(to, p, left);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
            p += written;
            left -= static_cast<size_t>(written);
        }
    }
}

int runClient(int client) {
    Request request{};
    if (!readAll(client, &request, sizeof(request)) || request.magic != kMagic ||
        request.format != kPcmS16Le || request.sample_rate < 8000 ||
        request.sample_rate > 192000 || request.channels == 0 || request.channels > 2 ||
        request.direction > kCapture) {
        return 1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) return 1;
    const pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }
    if (child == 0) {
        if (request.direction == kPlayback) {
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[1]);
        } else {
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[0]);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        char rate[16], channels[8];
        snprintf(rate, sizeof(rate), "%u", request.sample_rate);
        snprintf(channels, sizeof(channels), "%u", request.channels);
        if (request.direction == kPlayback) {
            execl("/odm/bin/pw-cat", "pw-cat", "--playback", "--raw", "--format", "s16",
                  "--rate", rate, "--channels", channels, "-", nullptr);
        } else {
            execl("/odm/bin/pw-cat", "pw-cat", "--record", "--raw", "--format", "s16",
                  "--rate", rate, "--channels", channels, "-", nullptr);
        }
        _exit(127);
    }

    close(request.direction == kPlayback ? pipefd[0] : pipefd[1]);
    const int media = request.direction == kPlayback ? pipefd[1] : pipefd[0];
    if (request.direction == kPlayback) {
        copyStream(client, media);
        shutdown(client, SHUT_RD);
        close(media);  // EOF lets pw-cat drain and exit.
    } else {
        copyStream(media, client);
        shutdown(client, SHUT_WR);
        close(media);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return 0;
}

}  // namespace

int main() {
    gIpc = matonos_ipc_create("audio");
    if (!gIpc || matonos_ipc_register(gIpc, "get_state", getState, nullptr) != 0 ||
        matonos_ipc_start(gIpc) != 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    unlink(kSocketPath);
    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) return 1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strlcpy(address.sun_path, kSocketPath, sizeof(address.sun_path));
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        chmod(kSocketPath, 0660) != 0 || listen(server, 8) != 0) {
        close(server);
        unlink(kSocketPath);
        return 1;
    }
    for (;;) {
        const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            usleep(100000);
            continue;
        }
        const pid_t child = fork();
        if (child == 0) {
            close(server);
            const int result = runClient(client);
            close(client);
            _exit(result);
        }
        close(client);
        if (child > 0) {
            while (waitpid(-1, nullptr, WNOHANG) > 0) {}
        }
    }
}
