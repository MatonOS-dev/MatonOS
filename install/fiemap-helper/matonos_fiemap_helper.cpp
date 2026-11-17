#include <libfiemap/image_manager.h>

#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>

#include <chrono>
#include <cstring>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
#define CLOSE_FD(fd_value) do { \
    const int close_fd_value = (fd_value); \
    if (close_fd_value >= 0) { \
        std::fprintf(stderr, "matonos-fiemap-helper: close fd=%d at %s:%d\\n", close_fd_value, __FILE__, __LINE__); \
        if (::close(close_fd_value) != 0) std::fprintf(stderr, "matonos-fiemap-helper: close fd=%d failed errno=%d at %s:%d\\n", close_fd_value, errno, __FILE__, __LINE__); \
    } \
} while (0)
constexpr char kMetadataDir[] = "/mnt/matonos-install/metadata/maton/images";
constexpr char kDataDir[] = "/mnt/matonos-install/system/images";
constexpr char kRequestPath[] = "/mnt/matonos-install/system/.matonos-fiemap-request";
constexpr char kResultPath[] = "/mnt/matonos-install/system/.matonos-fiemap-result";

bool ValidName(const char* value) {
    if (!value || !*value || std::strlen(value) > 36) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (!std::isalnum(*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

std::unique_ptr<android::fiemap::ImageManager> OpenManager() {
    return android::fiemap::ImageManager::Open(kMetadataDir, kDataDir);
}

int Fail(const char* message) {
    std::fprintf(stderr, "matonos-fiemap-helper: %s\n", message);
    return 1;
}

int Execute(const std::string& command, const std::string& name,
            const std::string& size_text, std::string* output) {
    if (command == "create") {
        char* end = nullptr;
        const unsigned long long size = std::strtoull(size_text.c_str(), &end, 10);
        if (!ValidName(name.c_str()) || !end || *end || size == 0) return 2;
        auto manager = OpenManager();
        if (!manager) return 3;
        return manager->CreateBackingImage(name,
                size, android::fiemap::IImageManager::CREATE_IMAGE_READONLY, nullptr) ? 0 : 4;
    }
    if (command == "map" || command == "unmap") {
        if (!ValidName(name.c_str())) return 2;
        auto manager = OpenManager();
        if (!manager) return 3;
        if (command == "map") {
            std::string path;
            if (!manager->MapImageDevice(name, std::chrono::seconds(10), &path)) return 4;
            *output = path;
            return 0;
        }
        return manager->UnmapImageDevice(name) ? 0 : 4;
    }
    if (command == "list") {
        auto manager = OpenManager();
        if (!manager) return 3;
        for (const auto& image : manager->GetAllBackingImages()) *output += image + "\n";
        return 0;
    }
    return 2;
}

int RunInitRequest() {
    std::ifstream request(kRequestPath);
    std::string id, command, name, size;
    if (!std::getline(request, id) || !std::getline(request, command) ||
        !std::getline(request, name) || !std::getline(request, size) || id.empty()) {
        return Fail("invalid init request file");
    }
    std::string output;
    const int status = Execute(command, name, size, &output);
    const std::string temp = std::string(kResultPath) + ".tmp";
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return Fail("open result file failed");
    const std::string result = id + "\n" + std::to_string(status) + "\n" + output;
    size_t offset = 0;
    while (offset < result.size()) {
        const ssize_t written = write(fd, result.data() + offset, result.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { CLOSE_FD(fd); return Fail("write result file failed"); }
        offset += static_cast<size_t>(written);
    }
    if (fsync(fd) != 0) { CLOSE_FD(fd); return Fail("fsync result file failed"); }
    CLOSE_FD(fd);
    if (rename(temp.c_str(), kResultPath) != 0) return Fail("rename result file failed");
    return status;
}
}

int main(int argc, char** argv) {
    if (argc == 1) return RunInitRequest();
    if (argc < 2) return Fail("expected create, map, unmap, or list");
    const std::string command(argv[1]);
    if (command == "create") {
        if (argc != 4 || !ValidName(argv[2])) return Fail("usage: create <name> <size-bytes>");
        char* end = nullptr;
        const unsigned long long size = std::strtoull(argv[3], &end, 10);
        if (!end || *end || size == 0) return Fail("image size must be a positive integer");
        std::string output;
        return Execute(command, argv[2], argv[3], &output) == 0 ? 0 : Fail("CreateBackingImage failed");
    }
    if (command == "map") {
        if (argc != 3 || !ValidName(argv[2])) return Fail("usage: map <name>");
        std::string output;
        if (Execute(command, argv[2], "0", &output) != 0) return Fail("MapImageDevice failed");
        std::puts(output.c_str()); return 0;
    }
    if (command == "unmap") {
        if (argc != 3 || !ValidName(argv[2])) return Fail("usage: unmap <name>");
        std::string output;
        return Execute(command, argv[2], "0", &output) == 0 ? 0 : Fail("UnmapImageDevice failed");
    }
    if (command == "list") {
        if (argc != 2) return Fail("usage: list");
        std::string output;
        if (Execute(command, "", "0", &output) != 0) return Fail("ImageManager::Open failed");
        std::fputs(output.c_str(), stdout);
        return 0;
    }
    return Fail("unsupported command");
}
