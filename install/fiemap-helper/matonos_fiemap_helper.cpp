#include <libfiemap/image_manager.h>
#include <fs_mgr_dm_linear.h>
#include <libdm/dm.h>
#include <liblp/liblp.h>
#include <liblp/partition_opener.h>

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
constexpr char kStoreName[] = "maton_system";
constexpr char kDevicePrefix[] = "maton-img-";
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
        // The live session already has dm devices named system_a etc., and
        // ImageManager names the dm device after the image. Map under a
        // private prefix instead (same dm-linear path as ImageManager), so the
        // image keeps the name first-stage init maps on the installed boot.
        const std::string device = std::string(kDevicePrefix) + name;
        if (command == "map") {
            const auto metadata = android::fs_mgr::ReadFromImageFile(std::string(kMetadataDir) + "/lp_metadata");
            if (!metadata) return 4;
            const auto* super = android::fs_mgr::GetMetadataSuperBlockDevice(*metadata);
            if (!super) return 4;
            // libfiemap records the backing partition's basename (nvme0n1p2);
            // there is no by-name link for it, so hand fs_mgr the /dev path.
            const std::string backing = android::fs_mgr::GetBlockDevicePartitionName(*super);
            if (!ValidName(backing.c_str())) return 4;
            android::fs_mgr::PartitionOpener opener;
            android::fs_mgr::CreateLogicalPartitionParams params = {
                    .block_device = "/dev/block/" + backing,
                    .metadata = metadata.get(),
                    .partition_name = name,
                    .force_writable = true,
                    .timeout_ms = std::chrono::seconds(10),
                    .device_name = device,
                    .partition_opener = &opener,
            };
            std::string path;
            if (!android::fs_mgr::CreateLogicalPartition(params, &path)) return 4;
            *output = path;
            return 0;
        }
        // Cleanup after a failed map: nothing mapped is already unmapped.
        if (android::dm::DeviceMapper::Instance().GetState(device) == android::dm::DmDeviceState::INVALID) return 0;
        return android::fs_mgr::DestroyLogicalPartition(device) ? 0 : 4;
    }
    if (command == "finalize") {
        // libfiemap records the backing partition's kernel name (nvme0n1p2):
        // the live session has no by-name link for the target drive. Kernel
        // names are not stable across boots, and first-stage init waits for
        // devices by GPT name, so persist the store's GPT name once all
        // images exist.
        if (name != kStoreName) return 2;
        const std::string file = std::string(kMetadataDir) + "/lp_metadata";
        auto metadata = android::fs_mgr::ReadFromImageFile(file);
        if (!metadata || metadata->block_devices.size() != 1) return 4;
        auto& device = metadata->block_devices[0];
        static_assert(sizeof(kStoreName) <= sizeof(device.partition_name));
        std::memset(device.partition_name, 0, sizeof(device.partition_name));
        std::memcpy(device.partition_name, kStoreName, sizeof(kStoreName) - 1);
        return android::fs_mgr::WriteToImageFile(file, *metadata) ? 0 : 4;
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
