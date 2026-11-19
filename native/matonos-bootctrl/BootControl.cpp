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
#include <cstdlib>
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

// Installed disks have no misc partition: the redundant state records are two
// files on the ESP, written alternately by generation.
const char* const kStateFiles[] = {"loader/matonos-bootctrl-0.bin", "loader/matonos-bootctrl-1.bin"};

bool ReadState(State* state) {
    if (IsLive()) { *state = DefaultState(); state->successful_mask = 1; return true; }
    mi::FatVolume volume;
    std::string error;
    if (!OpenCurrentEsp(false, &volume, &error)) {
        // Never hold early boot hostage to missing/late block metadata. This
        // default is read-only; slot changes still fail unless the ESP opens.
        *state = DefaultState();
        std::fprintf(stderr, "matonos-bootctrl: ESP unavailable (%s); using current-slot defaults\n", error.c_str());
        return true;
    }
    State candidates[2]{};
    bool valid[2]{};
    for (size_t i = 0; i < 2; ++i) {
        std::vector<uint8_t> bytes;
        valid[i] = volume.ReadFile(kStateFiles[i], &bytes, &error) && bytes.size() == sizeof(State);
        if (valid[i]) {
            std::memcpy(&candidates[i], bytes.data(), sizeof(State));
            valid[i] = Valid(candidates[i]);
        }
    }
    if (!valid[0] && !valid[1]) { *state = DefaultState(); return true; }
    *state = valid[1] && (!valid[0] || candidates[1].generation > candidates[0].generation)
                 ? candidates[1] : candidates[0];
    return true;
}

bool WriteState(State state) {
    if (IsLive()) return true;
    State current{};
    ReadState(&current);
    state.generation = current.generation + 1;
    std::memcpy(state.magic, kStateMagic, sizeof(kStateMagic));
    state.version = 1;
    state.crc = Crc32(&state, offsetof(State, crc));
    mi::FatVolume volume;
    std::string error;
    if (!OpenCurrentEsp(true, &volume, &error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot open ESP to save state: %s\n", error.c_str());
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(&state);
    if (!volume.WriteFile(kStateFiles[state.generation & 1u],
                          std::vector<uint8_t>(bytes, bytes + sizeof(state)), &error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot save state: %s\n", error.c_str());
        return false;
    }
    return true;
}

// Slot selection is entry ordering, not loader.conf `default`: systemd-boot
// 257 derives a counted entry's ID by stripping "+LEFT-DONE" (keeping case)
// while case-folding the pattern, and a pattern would keep matching an entry
// whose tries ran out. So loader.conf has no default; every slot entry carries
// the same sort-key plus a version, systemd-boot boots the highest version
// that still has tries, and exhausted (+0-N) entries sort last, which is the
// A/B fallback. Disabled or staged-but-not-activated entries are <stem>.DIS.
constexpr char kSortKey[] = "matonos";

std::string Stem(int slot) { return slot == 0 ? "A" : "B"; }

struct Entry {
    std::string name;
    std::string contents;
    int tries_left = -1;  // -1: no boot counter (marked successful)
};

bool ReadEntry(mi::FatVolume& volume, const std::string& name, std::string* contents) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!volume.ReadFile("loader/entries/" + name, &bytes, &error)) return false;
    contents->assign(bytes.begin(), bytes.end());
    return true;
}

std::vector<std::string> CountedNames(const std::string& stem) {
    std::vector<std::string> names;
    for (int left = 3; left >= 0; --left)
        for (int done = 0; done <= 3; ++done)
            names.push_back(stem + "+" + std::to_string(left) + "-" + std::to_string(done) + ".conf");
    return names;
}

// The slot's enabled entry: a counted one (most tries left first), else the plain one.
bool FindEnabledEntry(mi::FatVolume& volume, int slot, Entry* entry) {
    const std::string stem = Stem(slot);
    std::string contents;
    for (const std::string& name : CountedNames(stem)) {
        if (ReadEntry(volume, name, &contents)) {
            *entry = Entry{name, contents, name[stem.size() + 1] - '0'};
            return true;
        }
    }
    if (!ReadEntry(volume, stem + ".conf", &contents)) return false;
    *entry = Entry{stem + ".conf", contents, -1};
    return true;
}

bool HasLine(const std::string& contents, const std::string& key) {
    size_t begin = 0;
    while (begin < contents.size()) {
        const size_t end = contents.find('\n', begin);
        const std::string line = Trim(contents.substr(begin, (end == std::string::npos ? contents.size() : end) - begin));
        if (line.rfind(key + " ", 0) == 0) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

uint64_t EntryVersion(const std::string& contents) {
    size_t begin = 0;
    while (begin < contents.size()) {
        const size_t end = contents.find('\n', begin);
        const std::string line = Trim(contents.substr(begin, (end == std::string::npos ? contents.size() : end) - begin));
        if (line.rfind("version ", 0) == 0) return std::strtoull(Trim(line.substr(8)).c_str(), nullptr, 10);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return 0;
}

std::string WithOrdering(const std::string& contents, uint64_t version) {
    std::string updated;
    size_t begin = 0;
    while (begin < contents.size()) {
        const size_t end = contents.find('\n', begin);
        const std::string line = contents.substr(begin, (end == std::string::npos ? contents.size() : end) - begin);
        const std::string trimmed = Trim(line);
        if (!trimmed.empty() && trimmed.rfind("sort-key ", 0) != 0 && trimmed.rfind("version ", 0) != 0)
            updated += line + "\n";
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return updated + "sort-key " + kSortKey + "\nversion " + std::to_string(version) + "\n";
}

bool WriteEntry(mi::FatVolume& volume, const std::string& name, const std::string& contents, std::string* error) {
    return volume.WriteFile("loader/entries/" + name, std::vector<uint8_t>(contents.begin(), contents.end()), error);
}

bool RemoveEntryIfPresent(mi::FatVolume& volume, const std::string& name, std::string* error) {
    std::string ignored;
    if (!ReadEntry(volume, name, &ignored)) return true;
    return volume.RemoveFile("loader/entries/" + name, error);
}

// Removes every form of the slot's entry except `keep`.
bool RemoveSlotEntries(mi::FatVolume& volume, int slot, const std::string& keep, std::string* error) {
    const std::string stem = Stem(slot);
    std::vector<std::string> names = CountedNames(stem);
    names.push_back(stem + ".conf");
    names.push_back(stem + ".DIS");
    for (const std::string& name : names)
        if (name != keep && !RemoveEntryIfPresent(volume, name, error)) return false;
    return true;
}

bool DropLoaderDefault(mi::FatVolume& volume, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!volume.ReadFile("loader/loader.conf", &bytes, error)) return false;
    const std::string config(bytes.begin(), bytes.end());
    std::string updated;
    size_t begin = 0;
    while (begin < config.size()) {
        const size_t end = config.find('\n', begin);
        const std::string line = config.substr(begin, (end == std::string::npos ? config.size() : end) - begin);
        if (Trim(line).rfind("default ", 0) != 0) updated += line + "\n";
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (updated == config) return true;
    return volume.WriteFile("loader/loader.conf", std::vector<uint8_t>(updated.begin(), updated.end()), error);
}

// Mirrors systemd-boot's choice: highest version among entries with tries left.
bool ReadBootTarget(int* slot) {
    mi::FatVolume volume;
    std::string error;
    if (!OpenCurrentEsp(false, &volume, &error)) return false;
    int best = -1;
    uint64_t best_version = 0;
    for (int candidate = 0; candidate < 2; ++candidate) {
        Entry entry;
        if (!FindEnabledEntry(volume, candidate, &entry) || entry.tries_left == 0) continue;
        const uint64_t version = EntryVersion(entry.contents);
        if (best < 0 || version > best_version) { best = candidate; best_version = version; }
    }
    if (best < 0) return false;
    *slot = best;
    return true;
}

bool MarkEntrySuccessful(int slot) {
    mi::FatVolume volume;
    std::string error;
    if (!OpenCurrentEsp(true, &volume, &error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot open current ESP while marking slot %d successful: %s\n", slot, error.c_str());
        return false;
    }
    Entry entry;
    if (!FindEnabledEntry(volume, slot, &entry)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot find an enabled slot %s entry\n", Stem(slot).c_str());
        return false;
    }
    const std::string plain = Stem(slot) + ".conf";
    if ((entry.tries_left >= 0 &&
         (!WriteEntry(volume, plain, entry.contents, &error) || !volume.Sync(&error) ||
          !RemoveEntryIfPresent(volume, entry.name, &error))) ||
        !DropLoaderDefault(volume, &error) || !volume.Sync(&error)) {
        std::fprintf(stderr, "matonos-bootctrl: cannot mark slot %s entry successful: %s\n", Stem(slot).c_str(), error.c_str());
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
    if (ReadBootTarget(&from_loader)) { *result = from_loader; return ndk::ScopedAStatus::ok(); }
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
    const bool keep_successful = slot == CurrentSlot() && (state.successful_mask & (1u << slot)) != 0;
    {
        mi::FatVolume volume;
        std::string error;
        if (!OpenCurrentEsp(true, &volume, &error))
            return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot open systemd-boot ESP");
        const std::string stem = Stem(slot);
        Entry entry;
        if (!FindEnabledEntry(volume, slot, &entry)) {
            entry = Entry{stem + ".DIS", "", -1};
            if (!ReadEntry(volume, entry.name, &entry.contents))
                return Failure(boot::IBootControl::COMMAND_FAILED, "Slot has no boot entry");
        }
        // Give the other slot ordering fields if it predates them, then outrank it.
        uint64_t other_version = 0;
        Entry other;
        if (FindEnabledEntry(volume, 1 - slot, &other)) {
            other_version = EntryVersion(other.contents);
            if (!HasLine(other.contents, "sort-key") &&
                !WriteEntry(volume, other.name, WithOrdering(other.contents, other_version), &error)) {
                std::fprintf(stderr, "matonos-bootctrl: cannot order slot %s entry: %s\n", Stem(1 - slot).c_str(), error.c_str());
                return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot order other slot entry");
            }
        }
        const uint64_t version = std::max(other_version, EntryVersion(entry.contents)) + 1;
        // Re-activating the running, already successful slot needs no trial boots.
        const std::string name = keep_successful ? stem + ".conf" : stem + "+3-0.conf";
        if (!WriteEntry(volume, name, WithOrdering(entry.contents, version), &error) || !volume.Sync(&error) ||
            !RemoveSlotEntries(volume, slot, name, &error) || !DropLoaderDefault(volume, &error) ||
            !volume.Sync(&error)) {
            std::fprintf(stderr, "matonos-bootctrl: cannot activate slot %s entry: %s\n", stem.c_str(), error.c_str());
            return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot activate slot entry");
        }
    }
    state.bootable_mask |= 1u << slot;
    if (!keep_successful) state.successful_mask &= ~(1u << slot);
    state.active_slot = static_cast<uint32_t>(slot);
    return WriteState(state) ? ndk::ScopedAStatus::ok() : Failure(boot::IBootControl::COMMAND_FAILED, "Cannot persist slot state");
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
    if (state.active_slot == static_cast<uint32_t>(slot)) {
        const uint32_t other = static_cast<uint32_t>(1 - slot);
        if ((state.bootable_mask & (1u << other)) == 0)
            return Failure(boot::IBootControl::COMMAND_FAILED, "No bootable fallback slot");
        state.active_slot = other;
    }
    if (!WriteState(state)) return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot persist slot state");
    // A disabled entry is no longer a .conf, so systemd-boot falls back to the other slot.
    mi::FatVolume volume;
    std::string error;
    Entry entry;
    if (!OpenCurrentEsp(true, &volume, &error)) {
        (void)WriteState(previous); return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot open systemd-boot ESP");
    }
    if (FindEnabledEntry(volume, slot, &entry) &&
        (!WriteEntry(volume, Stem(slot) + ".DIS", entry.contents, &error) || !volume.Sync(&error) ||
         !RemoveSlotEntries(volume, slot, Stem(slot) + ".DIS", &error) || !volume.Sync(&error))) {
        std::fprintf(stderr, "matonos-bootctrl: cannot disable slot %s entry: %s\n", Stem(slot).c_str(), error.c_str());
        (void)WriteState(previous); return Failure(boot::IBootControl::COMMAND_FAILED, "Cannot disable slot entry");
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
