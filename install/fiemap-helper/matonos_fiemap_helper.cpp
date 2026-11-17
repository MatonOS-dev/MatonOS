#include <libfiemap/image_manager.h>

#include <android-base/unique_fd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
constexpr char kMetadataDir[] = "/mnt/matonos-install/metadata/maton/images";
constexpr char kDataDir[] = "/mnt/matonos-install/system/images";

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
}

int main(int argc, char** argv) {
    if (argc < 2) return Fail("expected create, map, unmap, or list");
    const std::string command(argv[1]);
    if (command == "create") {
        if (argc != 4 || !ValidName(argv[2])) return Fail("usage: create <name> <size-bytes>");
        char* end = nullptr;
        const unsigned long long size = std::strtoull(argv[3], &end, 10);
        if (!end || *end || size == 0) return Fail("image size must be a positive integer");
        auto manager = OpenManager();
        if (!manager) return Fail("ImageManager::Open failed for the install image directories");
        const auto status = manager->CreateBackingImage(argv[2], size,
                android::fiemap::IImageManager::CREATE_IMAGE_READONLY, nullptr);
        if (!status) return Fail("CreateBackingImage failed");
        return 0;
    }
    if (command == "map") {
        if (argc != 3 || !ValidName(argv[2])) return Fail("usage: map <name>");
        auto manager = OpenManager();
        if (!manager) return Fail("ImageManager::Open failed for the install image directories");
        std::string path;
        if (!manager->MapImageDevice(argv[2], std::chrono::seconds(10), &path))
            return Fail("MapImageDevice failed");
        std::puts(path.c_str());
        return 0;
    }
    if (command == "unmap") {
        if (argc != 3 || !ValidName(argv[2])) return Fail("usage: unmap <name>");
        auto manager = OpenManager();
        if (!manager) return Fail("ImageManager::Open failed for the install image directories");
        if (!manager->UnmapImageDevice(argv[2])) return Fail("UnmapImageDevice failed");
        return 0;
    }
    if (command == "list") {
        if (argc != 2) return Fail("usage: list");
        auto manager = OpenManager();
        if (!manager) return Fail("ImageManager::Open failed for the install image directories");
        for (const auto& name : manager->GetAllBackingImages()) std::puts(name.c_str());
        return 0;
    }
    return Fail("unsupported command");
}
