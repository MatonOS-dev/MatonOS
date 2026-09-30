#include "BootControl.h"

#include "FatFilesystem.h"

#include <sys/system_properties.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>

namespace mi = matonos::install;
namespace boot = aidl::android::hardware::boot;
namespace fs = std::filesystem;
namespace matonos::bootctrl {
namespace {
std::mutex g_state_mutex;

// Keep Android's bootloader_message and bootloader_control area (first 4 KiB)
// intact; the MatonOS redundant state records start immediately after it.
constexpr uint64_t kStateOffsets[] = {4096, 8192};
constexpr char kStateMagic[8] = {'M','A','T','O','N','B','C','1'};
#pragma pack(push, 1)
struct State {
    char magic[8];
    uint32_t version;
    uint32_t generation;
    uint32_t active_slot;
    uint32_t bootable_mask;
    uint32_t successful_mask;
    uint32_t merge_status;
    uint32_t crc;
};
#pragma pack(pop)

uint32_t Crc32(const void* value, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(value);
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

std::string Trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

bool Valid(const State& state) {
    return std::memcmp(state.magic, kStateMagic, sizeof(kStateMagic)) == 0 &&
           state.version == 1 && state.active_slot < 2 &&
           (state.bootable_mask & ~3u) == 0 && (state.successful_mask & ~3u) == 0 &&
           state.merge_status <= static_cast<uint32_t>(boot::MergeStatus::CANCELLED) &&
           state.crc == Crc32(&state, offsetof(State, crc));
}

int CurrentSlot() {
    char suffix[PROP_VALUE_MAX]{};
    const int size = __system_property_get("ro.boot.slot_suffix", suffix);
    if (size <= 0) return 0;
    if (strcmp(suffix, "_a") == 0) return 0;
    if (strcmp(suffix, "_b") == 0) return 1;
    return 0;
}

bool IsLive() {
    char value[PROP_VALUE_MAX]{};
    return __system_property_get("ro.boot.matonos.live", value) > 0 && strcmp(value, "1") == 0;
}

std::string CurrentDiskSysPath() {
    char boot_part_uuid[PROP_VALUE_MAX]{};
    if (__system_property_get("ro.boot.boot_part_uuid", boot_part_uuid) > 0) {
        std::error_code ec;
        for (fs::directory_iterator it("/sys/class/block", ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path node = fs::canonical(it->path(), ec);
            if (ec) { ec.clear(); continue; }
            std::error_code partition_ec; if (!fs::exists(node / "partition", partition_ec) || partition_ec) continue;
            std::ifstream uevent(it->path() / "uevent");
            std::string line;
            while (std::getline(uevent, line)) {
                if (line == std::string("PARTUUID=") + boot_part_uuid)
                    return fs::canonical(node.parent_path(), ec).string();
            }
        }
    }
    // Resolve the active physical disk from its GPT PARTNAME when the boot PARTUUID is unavailable.
    const std::string active_name = std::string("system") + (CurrentSlot() == 0 ? "_a" : "_b");
    std::error_code ec;
    for (fs::directory_iterator it("/sys/class/block", ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path node = fs::canonical(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        std::error_code partition_ec;
        if (!fs::exists(node / "partition", partition_ec) || partition_ec) continue;
        std::ifstream input(it->path() / "uevent");
        if (!input) continue;
        std::string line;
        while (std::getline(input, line)) {
            if (line == "PARTNAME=" + active_name) {
                const fs::path parent = fs::canonical(node.parent_path(), ec);
                if (!ec) return parent.string();
                ec.clear();
                break;
            }
        }
    }
    std::fprintf(stderr, "matonos-bootctrl: cannot resolve physical disk for active PARTNAME=%s\n", active_name.c_str());
    return {};
}

std::string FindCurrentDiskPartition(const std::string& wanted_name) {
    const std::string disk=CurrentDiskSysPath();
    if(disk.empty())return {};
    std::error_code ec;
    for(fs::directory_iterator it("/sys/class/block",ec),end;!ec&&it!=end;it.increment(ec)){
        const fs::path sys=fs::canonical(it->path(),ec);if(ec){ec.clear();continue;}
        std::error_code partition_ec; if(!fs::exists(sys/"partition",partition_ec)||partition_ec||fs::canonical(sys.parent_path(),ec).string()!=disk){ec.clear();continue;}
        std::ifstream input(it->path()/"uevent");if(!input)continue;std::string line;
        while(std::getline(input,line))if(line=="PARTNAME="+wanted_name){
            const std::string node="/dev/block/"+it->path().filename().string();
            if(access(node.c_str(),R_OK)==0)return node;
            std::fprintf(stderr, "matonos-bootctrl: cannot read %s: %s\n", node.c_str(), strerror(errno));
            break;
        }
    }
    std::fprintf(stderr, "matonos-bootctrl: PARTNAME=%s not found on %s\n", wanted_name.c_str(), disk.c_str());
    return {};
}

bool OpenCurrentEsp(bool writable, mi::FatVolume* volume, std::string* error) {
    const std::string esp = FindCurrentDiskPartition("esp");
    if (esp.empty()) {
        *error = "Current disk ESP is unavailable.";
        return false;
    }
    uint64_t disk_sequence = 0;
    if (writable) {
        const std::string disk = CurrentDiskSysPath();
        if (disk.empty()) {
            *error = "Current physical disk is unavailable.";
            return false;
        }
        const std::string disk_node = "/dev/block/" + fs::path(disk).filename().string();
        const int fd = open(disk_node.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            *error = "Cannot open current physical disk for identity check.";
            return false;
        }
        const int sequence_rc = ioctl(fd, BLKGETDISKSEQ, &disk_sequence);
        const int sequence_error = errno;
        close(fd);
        // Older kernels may not implement BLKGETDISKSEQ; the ESP opener can still validate its geometry.
        if (sequence_rc != 0 && sequence_error != ENOTTY && sequence_error != EINVAL) { *error = "Cannot verify current physical disk sequence."; return false; }
        if (sequence_rc != 0) disk_sequence = 0;
    }
    return mi::FatVolume::Open(esp, writable, volume, error, disk_sequence);
}

State DefaultState() {
    State state{};
    std::memcpy(state.magic, kStateMagic, sizeof(kStateMagic));
    state.version = 1;
    state.active_slot = static_cast<uint32_t>(CurrentSlot());
    state.bootable_mask = 3;
    state.successful_mask = 0;
    state.merge_status = static_cast<uint32_t>(boot::MergeStatus::NONE);
    state.crc = Crc32(&state, offsetof(State, crc));
    return state;
}

bool ReadState(State* state) {
    if (IsLive()) { *state = DefaultState(); state->successful_mask = 1; return true; }
    const std::string path = FindCurrentDiskPartition("misc");
    if (path.empty()) {
        // Never hold early boot hostage to missing/late block metadata. This
        // default is read-only; slot changes still fail unless misc is found.
        *state = DefaultState();
        std::fprintf(stderr, "matonos-bootctrl: misc unavailable; using current-slot defaults\n");
        return true;
    }
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        *state = DefaultState();
        std::fprintf(stderr, "matonos-bootctrl: cannot open %s: %s; using defaults\n", path.c_str(), strerror(errno));
        return true;
    }
    State candidates[2]{};
    bool valid[2]{};
    for (size_t i = 0; i < 2; ++i) {
        valid[i] = pread(fd, &candidates[i], sizeof(State), kStateOffsets[i]) == sizeof(State) &&
                   Valid(candidates[i]);
    }
    close(fd);
    if (!valid[0] && !valid[1]) { *state = DefaultState(); return true; }
    *state = valid[1] && (!valid[0] || candidates[1].generation > candidates[0].generation)
                 ? candidates[1] : candidates[0];
    return true;
}

bool WriteState(State state) {
    if (IsLive()) return true;
    const std::string path = FindCurrentDiskPartition("misc");
    if (path.empty()) return false;
    State current{};
    ReadState(&current);
    state.generation = current.generation + 1;
    std::memcpy(state.magic, kStateMagic, sizeof(kStateMagic));
    state.version = 1;
    state.crc = Crc32(&state, offsetof(State, crc));
    const uint64_t offset = kStateOffsets[state.generation & 1u];
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < sizeof(state)) {
        const ssize_t count = pwrite(fd, reinterpret_cast<const uint8_t*>(&state) + done, sizeof(state) - done, static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += static_cast<size_t>(count);
    }
    const bool ok = done == sizeof(state) && fsync(fd) == 0;
    close(fd);
    return ok;
}

bool ReadLoaderDefault(int* slot) {
    const std::string path = FindCurrentDiskPartition("esp");
    if (path.empty()) return false;
    mi::FatVolume volume;
    std::string error;
    if (!mi::FatVolume::Open(path, false, &volume, &error)) return false;
    std::vector<uint8_t> bytes;
    if (!volume.ReadFile("loader/loader.conf", &bytes, &error)) return false;
    const std::string config(bytes.begin(), bytes.end());
    size_t begin = 0;
    while (begin <= config.size()) {
        const size_t end = config.find('\n', begin);
        const std::string line = config.substr(begin,
                (end == std::string::npos ? config.size() : end) - begin);
        const std::string trimmed = Trim(line);
        if (trimmed.rfind("default ", 0) == 0) {
            const std::string entry = Trim(trimmed.substr(8));
            if (entry.rfind("A",0)==0) { *slot = 0; return true; }
            if (entry.rfind("B",0)==0) { *slot = 1; return true; }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

bool WriteLoaderDefault(int slot) {
    mi::FatVolume volume;
    std::string error;
    if (!OpenCurrentEsp(true, &volume, &error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot open ESP to select slot %d: %s\n", slot, error.c_str());
        return false;
    }
    std::vector<uint8_t> old_bytes;
    if (!volume.ReadFile("loader/loader.conf", &old_bytes, &error)) return false;
    std::string config(old_bytes.begin(), old_bytes.end());
    bool replaced = false;
    std::string updated;
    size_t begin = 0;
    while (begin < config.size()) {
        const size_t end = config.find('\n', begin);
        const size_t length = (end == std::string::npos ? config.size() : end) - begin;
        const std::string line = config.substr(begin, length);
        const std::string trimmed = Trim(line);
        if (!replaced && trimmed.rfind("default ", 0) == 0) {
            updated += "default matonos-" + std::string(slot == 0 ? "a" : "b") + ".conf";
            replaced = true;
        } else {
            updated += line;
        }
        if (end != std::string::npos) updated.push_back('\n');
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    const std::string stem = slot == 0 ? "A" : "B";
    std::string selected = stem + ".conf";
    for (int left=3; left>=0; --left) {
        for (int done=0; done<=3; ++done) {
            const std::string candidate=stem+"+"+std::to_string(left)+"-"+std::to_string(done)+".conf";
            std::vector<uint8_t> ignored;
            if (volume.ReadFile("loader/entries/"+candidate,&ignored,&error)) { selected=candidate; left=-1; break; }
        }
    }
    if (!replaced) updated = "default " + selected + "\n" + config;
    else {
        const std::string old_stem="default ";
        size_t pos=updated.find(old_stem);
        if(pos!=std::string::npos){size_t end=updated.find('\n',pos);updated.replace(pos,end==std::string::npos?updated.size()-pos:end-pos,"default "+selected);}
    }
    const std::vector<uint8_t> output(updated.begin(), updated.end());
    return volume.WriteFile("loader/loader.conf", output, &error);
}

bool MarkEntrySuccessful(int slot) {
    mi::FatVolume volume;std::string error;
    if(!OpenCurrentEsp(true,&volume,&error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot open current ESP while marking slot %d successful: %s\n", slot, error.c_str());
        return false;
    }
    const std::string stem=slot==0?"A":"B";
    std::vector<uint8_t> contents;
    std::string selected;
    for(int left=3;left>=0&&selected.empty();--left)for(int done=0;done<=3;++done){
        const std::string candidate=stem+"+"+std::to_string(left)+"-"+std::to_string(done)+".conf";
        if(volume.ReadFile("loader/entries/"+candidate,&contents,&error)){selected=candidate;break;}
    }
    if(selected.empty()) {
        const bool ok = volume.ReadFile("loader/entries/"+stem+".conf",&contents,&error);
        if (!ok) std::fprintf(stderr, "matonos-bootctrl: cannot find slot %s entry: %s\n", stem.c_str(), error.c_str());
        return ok;
    }
    if(!volume.WriteFile("loader/entries/"+stem+".conf",contents,&error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot write successful slot %s entry: %s\n", stem.c_str(), error.c_str());
        return false;
    }
    if(!volume.RenameFile("loader/entries/"+selected,"loader/entries/"+stem+".DIS",&error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot retire boot-count entry %s: %s\n", selected.c_str(), error.c_str());
        return false;
    }
    if (!WriteLoaderDefault(slot)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot update systemd-boot default for slot %s\n", stem.c_str());
        return false;
    }
    return true;
}

ndk::ScopedAStatus Failure(int code, const char* message) {
    return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(code, message);
}

}  // namespace

ndk::ScopedAStatus BootControl::getActiveBootSlot(int32_t* result) {
    if (IsLive()) { *result = 0; return ndk::ScopedAStatus::ok(); }
    int from_loader = -1;
    if (ReadLoaderDefault(&from_loader)) { *result = from_loader; return ndk::ScopedAStatus::ok(); }
    State state{};
    if (!ReadState(&state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read slot state");
    *result = static_cast<int32_t>(state.active_slot);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::getCurrentSlot(int32_t* result) {
    *result = CurrentSlot();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::getNumberSlots(int32_t* result) {
    *result = IsLive() ? 1 : 2;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::getSnapshotMergeStatus(boot::MergeStatus* result) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (IsLive()) { *result = boot::MergeStatus::NONE; return ndk::ScopedAStatus::ok(); }
    State state{};
    if (!ReadState(&state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read snapshot merge state");
    *result = static_cast<boot::MergeStatus>(state.merge_status);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::getSuffix(int32_t slot, std::string* result) {
    if (slot < 0 || slot >= (IsLive() ? 1 : 2)) { result->clear(); return ndk::ScopedAStatus::ok(); }
    *result = slot == 0 ? "_a" : "_b";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::isSlotBootable(int32_t slot, bool* result) {
    if (slot < 0 || slot >= (IsLive() ? 1 : 2)) return Failure(boot::IBootControl::INVALID_SLOT, "Invalid slot");
    if (IsLive()) { *result = true; return ndk::ScopedAStatus::ok(); }
    State state{};
    if (!ReadState(&state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read slot state");
    *result = (state.bootable_mask & (1u << slot)) != 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::isSlotMarkedSuccessful(int32_t slot, bool* result) {
    if (slot < 0 || slot >= (IsLive() ? 1 : 2)) return Failure(boot::IBootControl::INVALID_SLOT, "Invalid slot");
    if (IsLive()) { *result = true; return ndk::ScopedAStatus::ok(); }
    State state{};
    if (!ReadState(&state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read slot state");
    *result = (state.successful_mask & (1u << slot)) != 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::markBootSuccessful() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (IsLive()) return ndk::ScopedAStatus::ok();
    State state{};
    if (!ReadState(&state)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot read slot state while marking current slot successful\n");
        return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read slot state");
    }
    const int slot = CurrentSlot();
    const State previous = state;
    state.bootable_mask |= 1u << slot;
    state.successful_mask |= 1u << slot;
    if (!WriteState(state)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot persist successful slot %d state to misc\n", slot);
        return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot persist slot state");
    }
    if (!MarkEntrySuccessful(slot)) { (void)WriteState(previous); return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot mark systemd-boot entry successful"); }
    std::fprintf(stderr, "matonos-bootctrl: marked slot %d successful\n", slot);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::setActiveBootSlot(int32_t slot) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (slot < 0 || slot >= (IsLive() ? 1 : 2)) return Failure(boot::IBootControl::INVALID_SLOT, "Invalid slot");
    if (IsLive()) return ndk::ScopedAStatus::ok();
    State state{};
    if (!ReadState(&state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read slot state");
    if (state.merge_status == static_cast<uint32_t>(boot::MergeStatus::MERGING))
        return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot switch slots while snapshot merge is active");
    {
        mi::FatVolume volume;std::string error;
        if(!OpenCurrentEsp(true,&volume,&error))
            return Failure(boot::IBootControl::COMMAND_FAILED,"Cannot open systemd-boot ESP");
        const std::string stem=slot==0?"A":"B";
        std::vector<uint8_t> disabled;
        if(volume.ReadFile("loader/entries/"+stem+".DIS",&disabled,&error)) {
            if(!volume.WriteFile("loader/entries/"+stem+"+3-0.conf",disabled,&error))
                return Failure(boot::IBootControl::COMMAND_FAILED,"Cannot restore slot entry");
            if(!volume.RenameFile("loader/entries/"+stem+".DIS","loader/entries/"+stem+".OLD",&error))
                return Failure(boot::IBootControl::COMMAND_FAILED,"Cannot restore slot entry");
        }
        std::vector<uint8_t> successful;
        if(volume.ReadFile("loader/entries/"+stem+".conf",&successful,&error) &&
           (!volume.WriteFile("loader/entries/"+stem+"+3-0.conf",successful,&error) ||
            !volume.RenameFile("loader/entries/"+stem+".conf","loader/entries/"+stem+".BAK",&error)))
            return Failure(boot::IBootControl::COMMAND_FAILED,"Cannot reset slot boot counter");
    }
    state.bootable_mask |= 1u << slot;
    state.successful_mask &= ~(1u << slot);
    state.active_slot = static_cast<uint32_t>(slot);
    if (!WriteState(state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot persist slot state");
    return WriteLoaderDefault(slot) ? ndk::ScopedAStatus::ok() : Failure(boot::IBootControl::COMMAND_FAILED, "Cannot select systemd-boot entry");
}

ndk::ScopedAStatus BootControl::setSlotAsUnbootable(int32_t slot) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (slot < 0 || slot >= (IsLive() ? 1 : 2)) return Failure(boot::IBootControl::INVALID_SLOT, "Invalid slot");
    if (IsLive()) return Failure(boot::IBootControl::COMMAND_FAILED, "Live slot cannot be disabled");
    State state{};
    if (!ReadState(&state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read slot state");
    if (state.merge_status == static_cast<uint32_t>(boot::MergeStatus::MERGING))
        return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot disable a slot while snapshot merge is active");
    const State previous = state;
    state.bootable_mask &= ~(1u << slot);
    state.successful_mask &= ~(1u << slot);
    const bool change_default = state.active_slot == static_cast<uint32_t>(slot);
    if (change_default) {
        const uint32_t other = static_cast<uint32_t>(1 - slot);
        if ((state.bootable_mask & (1u << other)) == 0)
            return Failure(boot::IBootControl::COMMAND_FAILED, "No bootable fallback slot");
        state.active_slot = other;
    }
    if (!WriteState(state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot persist slot state");
    auto rollback = [&]() { (void)WriteState(previous); };
    {
        mi::FatVolume volume; std::string error;
        if (!OpenCurrentEsp(true, &volume, &error)) {
            rollback(); return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot open systemd-boot ESP");
        }
        const std::string stem = slot == 0 ? "A" : "B";
        std::vector<uint8_t> entry; std::string selected;
        for (int left = 3; left >= 0 && selected.empty(); --left) for (int done = 0; done <= 3; ++done) {
            const std::string candidate = stem + "+" + std::to_string(left) + "-" + std::to_string(done) + ".conf";
            if (volume.ReadFile("loader/entries/" + candidate, &entry, &error)) { selected = candidate; break; }
        }
        if (selected.empty() && volume.ReadFile("loader/entries/" + stem + ".conf", &entry, &error)) selected = stem + ".conf";
        if (!selected.empty() && !volume.RenameFile("loader/entries/" + selected, "loader/entries/" + stem + ".DIS", &error)) {
            rollback(); return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot disable slot entry");
        }
    }
    if (change_default && !WriteLoaderDefault(static_cast<int>(state.active_slot))) {
        rollback(); return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot select fallback systemd-boot entry");
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BootControl::setSnapshotMergeStatus(boot::MergeStatus status) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const int32_t value = static_cast<int32_t>(status);
    if (value < static_cast<int32_t>(boot::MergeStatus::NONE) ||
        value > static_cast<int32_t>(boot::MergeStatus::CANCELLED))
        return Failure(boot::IBootControl::COMMAND_FAILED, "Invalid snapshot merge status");
    if (IsLive()) return ndk::ScopedAStatus::ok();
    State state{};
    if (!ReadState(&state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot read slot state");
    state.merge_status = static_cast<uint32_t>(status);
    return WriteState(state) ? ndk::ScopedAStatus::ok() : Failure(boot::IBootControl::COMMAND_FAILED, "Cannot persist slot state");
}

}  // namespace matonos::bootctrl
