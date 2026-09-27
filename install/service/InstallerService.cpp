#include "InstallerService.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace matonos::install {
namespace fs = std::filesystem;
namespace {

std::string Read(const fs::path& path) {
    std::ifstream input(path);
    std::string value((std::istreambuf_iterator<char>(input)), {});
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    return value.substr(start);
}

bool ReadBool(const fs::path& path) { return Read(path) == "1"; }

bool ParseDev(const std::string& text, unsigned* maj, unsigned* min) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) return false;
    try {
        *maj = static_cast<unsigned>(std::stoul(text.substr(0, colon)));
        *min = static_cast<unsigned>(std::stoul(text.substr(colon + 1)));
        return true;
    } catch (...) {
        return false;
    }
}

std::string DevId(unsigned maj, unsigned min) {
    return std::to_string(maj) + ":" + std::to_string(min);
}

bool IsVirtualOrOptical(const std::string& name) {
    return name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 ||
           name.rfind("zram", 0) == 0 || name.rfind("sr", 0) == 0 ||
           name.rfind("fd", 0) == 0 || name.rfind("dm-", 0) == 0 ||
           name.find("boot0") != std::string::npos || name.find("boot1") != std::string::npos;
}

std::set<std::string> PhysicalDeviceIds(const fs::path& sys_disk) {
    std::set<std::string> ids;
    auto add = [&](const fs::path& entry) {
        unsigned maj = 0, min = 0;
        if (ParseDev(Read(entry / "dev"), &maj, &min)) ids.insert(DevId(maj, min));
    };
    add(sys_disk);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sys_disk, ec)) {
        if (ec) break;
        if (fs::exists(entry.path() / "partition", ec)) add(entry.path());
    }
    return ids;
}

std::set<std::string> MountedDeviceClosure(const fs::path& sys_block,
                                          const std::string& mountinfo) {
    std::set<std::string> pending;
    std::set<std::string> seen;
    std::ifstream input(mountinfo);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string token;
        // mountinfo: id parent major:minor root mountpoint ...
        if (!(fields >> token >> token >> token)) continue;
        pending.insert(token);
    }

    while (!pending.empty()) {
        const std::string id = *pending.begin();
        pending.erase(pending.begin());
        if (!seen.insert(id).second) continue;
        unsigned maj = 0, min = 0;
        if (!ParseDev(id, &maj, &min)) continue;
        std::error_code ec;
        const fs::path device = fs::canonical(sys_block.parent_path() / "dev" / "block" / id, ec);
        if (ec) continue;
        const fs::path slaves = device / "slaves";
        for (const auto& slave : fs::directory_iterator(slaves, ec)) {
            if (ec) break;
            unsigned slave_maj = 0, slave_min = 0;
            if (ParseDev(Read(slave.path() / "dev"), &slave_maj, &slave_min))
                pending.insert(DevId(slave_maj, slave_min));
        }
    }
    return seen;
}

std::set<std::string> ActiveSwapIds(const std::string& swapinfo) {
    std::set<std::string> ids;
    std::ifstream input(swapinfo);
    std::string filename, type, size, used, priority;
    if (!(input >> filename >> type >> size >> used >> priority)) return ids;
    while (input >> filename >> type >> size >> used >> priority) {
        struct stat st {};
        if (::stat(filename.c_str(), &st) == 0 && S_ISBLK(st.st_mode))
            ids.insert(DevId(major(st.st_rdev), minor(st.st_rdev)));
    }
    return ids;
}

bool HasHolders(const fs::path& sys_disk) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sys_disk, ec)) {
        if (ec) break;
        if (!fs::exists(entry.path() / "partition", ec)) continue;
        const fs::path holders = entry.path() / "holders";
        for (const auto& holder : fs::directory_iterator(holders, ec)) {
            (void)holder;
            if (!ec) return true;
            ec.clear();
        }
    }
    const fs::path holders = sys_disk / "holders";
    for (const auto& holder : fs::directory_iterator(holders, ec)) {
        (void)holder;
        if (!ec) return true;
        ec.clear();
    }
    return false;
}

std::string ReadLivePartUuid(const std::string& cmdline) {
    std::ifstream input(cmdline);
    std::string line;
    std::getline(input, line);
    std::istringstream words(line);
    std::string word;
    constexpr std::string_view prefix = "androidboot.boot_part_uuid=";
    while (words >> word) {
        if (word.rfind(prefix, 0) == 0) return word.substr(prefix.size());
    }
    return {};
}

bool DiskHasPartUuid(const fs::path& sys_disk, const std::string& uuid) {
    if (uuid.empty()) return false;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sys_disk, ec)) {
        if (ec) break;
        if (!fs::exists(entry.path() / "partition", ec)) continue;
        std::ifstream uevent(entry.path() / "uevent");
        std::string line;
        while (std::getline(uevent, line)) {
            if (line.rfind("PARTUUID=", 0) == 0 && line.substr(9) == uuid) return true;
        }
    }
    return false;
}

std::string Transport(const std::string& sys_path, const std::string& name) {
    if (name.rfind("nvme", 0) == 0) return "NVMe";
    if (name.rfind("mmcblk", 0) == 0) return "MMC";
    if (sys_path.find("/usb") != std::string::npos) return "USB";
    if (sys_path.find("/ata") != std::string::npos || sys_path.find("/scsi") != std::string::npos)
        return "SATA/SCSI";
    return "Storage";
}

std::string UnsafeReason(const Drive& drive) {
    if (drive.live_medium) return "This is the drive MatonOS booted from.";
    if (drive.mounted) return "One or more partitions on this drive are mounted or in use.";
    if (drive.in_use) return "This drive has an active block-device holder.";
    if (drive.read_only) return "This drive is read-only.";
    return {};
}

bool SafeRelative(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos) return false;
    fs::path parsed(path);
    for (const auto& component : parsed) if (component == ".." || component == ".") return false;
    return !parsed.has_root_path();
}

bool ValidGuid(const std::string& guid) {
    if (guid.size() != 36) return false;
    for (size_t i = 0; i < guid.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (guid[i] != '-') return false; }
        else if (!std::isxdigit(static_cast<unsigned char>(guid[i]))) return false;
    }
    return true;
}

const Partition* FindPartition(const Drive& drive, const PartitionRef& ref) {
    if (ref.part_guid.empty() == ref.logical_name.empty()) return nullptr;
    const auto it = std::find_if(drive.partitions.begin(), drive.partitions.end(), [&](const Partition& p) {
        return ref.logical_name.empty() ? p.part_guid == ref.part_guid : p.logical && p.name == ref.logical_name;
    });
    return it == drive.partitions.end() ? nullptr : &*it;
}

bool CheckRef(const Drive& drive, const PartitionRef& ref) {
    const Partition* part = FindPartition(drive, ref);
    return part && !part->mounted && part->size_bytes != 0 &&
           part->start_bytes <= drive.size_bytes && part->size_bytes <= drive.size_bytes - part->start_bytes;
}

bool CheckGpt(const WriteGpt& gpt, const Drive& disk, std::string* error) {
    if (gpt.declared_disk_bytes != disk.size_bytes || !ValidGuid(gpt.disk_guid) || gpt.partitions.empty()) {
        *error = "GPT description does not match the freshly enumerated target disk."; return false;
    }
    std::set<std::string> names, guids;
    guids.insert(gpt.disk_guid);
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& p : gpt.partitions) {
        if (p.name.empty() || p.name.size() > 36 || !ValidGuid(p.type_guid) || !ValidGuid(p.part_guid) ||
            !names.insert(p.name).second || !guids.insert(p.part_guid).second || p.size_bytes == 0 ||
            p.start_bytes % (1024 * 1024) != 0 || p.start_bytes > disk.size_bytes ||
            p.size_bytes > disk.size_bytes - p.start_bytes ||
            p.start_bytes + p.size_bytes > disk.size_bytes - std::min<uint64_t>(disk.size_bytes, 34 * 512ULL)) {
            *error = "GPT contains an invalid, duplicate, unaligned, or out-of-bounds partition."; return false;
        }
        ranges.emplace_back(p.start_bytes, p.start_bytes + p.size_bytes);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1].second) {
        *error = "GPT partitions overlap."; return false;
    }
    return true;
}

}  // namespace

std::vector<Drive> EnumerateDrives(const std::string& sys_block_path,
                                   const std::string& dev_block_path,
                                   const std::string& mountinfo,
                                   const std::string& cmdline,
                                   const std::string& swapinfo) {
    std::vector<Drive> drives;
    const fs::path sys_block(sys_block_path);
    auto mounted = MountedDeviceClosure(sys_block, mountinfo);
    const auto swaps = ActiveSwapIds(swapinfo);
    mounted.insert(swaps.begin(), swaps.end());
    const std::string live_uuid = ReadLivePartUuid(cmdline);
    std::ifstream mountinfo_check(mountinfo);
    std::string mount_probe;
    const bool mountinfo_valid = mountinfo_check && static_cast<bool>(mountinfo_check >> mount_probe);
    std::ifstream swapinfo_check(swapinfo);
    const bool swapinfo_valid = swapinfo_check && static_cast<bool>(swapinfo_check >> mount_probe);
    const bool live_uuid_valid = !live_uuid.empty();
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(sys_block, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (IsVirtualOrOptical(name)) continue;

        unsigned maj = 0, min = 0;
        if (!ParseDev(Read(entry.path() / "dev"), &maj, &min)) continue;
        const fs::path node = fs::path(dev_block_path) / name;
        struct stat st {};
        if (::stat(node.c_str(), &st) != 0 || !S_ISBLK(st.st_mode) ||
            major(st.st_rdev) != maj || minor(st.st_rdev) != min) continue;

        Drive drive;
        drive.id = DevId(maj, min);
        std::error_code node_error;
        drive.path = fs::canonical(node, node_error).string();
        if (node_error) continue;
        drive.size_bytes = std::strtoull(Read(entry.path() / "size").c_str(), nullptr, 10) * 512ULL;
        drive.read_only = ReadBool(entry.path() / "ro");
        drive.removable = ReadBool(entry.path() / "removable");
        drive.model = Read(entry.path() / "device" / "model");
        if (drive.model.empty()) drive.model = name;

        std::error_code real_error;
        const std::string real_sys = fs::canonical(entry.path(), real_error).string();
        drive.transport = Transport(real_sys, name);
        const auto physical_ids = PhysicalDeviceIds(entry.path());
        drive.mounted = std::any_of(physical_ids.begin(), physical_ids.end(),
                                    [&](const std::string& id) { return mounted.count(id) != 0; });
        drive.in_use = HasHolders(entry.path());
        for (const auto& child : fs::directory_iterator(entry.path(), ec)) {
            if (ec) break;
            if (!fs::exists(child.path() / "partition", ec)) continue;
            Partition partition;
            partition.start_bytes = std::strtoull(Read(child.path() / "start").c_str(), nullptr, 10) * 512ULL;
            partition.size_bytes = std::strtoull(Read(child.path() / "size").c_str(), nullptr, 10) * 512ULL;
            unsigned pmaj = 0, pmin = 0;
            if (ParseDev(Read(child.path() / "dev"), &pmaj, &pmin))
                partition.mounted = mounted.count(DevId(pmaj, pmin)) != 0;
            std::ifstream uevent(child.path() / "uevent");
            std::string line;
            while (std::getline(uevent, line)) {
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const auto key = line.substr(0, eq), value = line.substr(eq + 1);
                if (key == "PARTNAME") partition.name = value;
                else if (key == "PARTUUID") partition.part_guid = value;
                else if (key == "PARTTYPE") partition.type_guid = value;
            }
            if (partition.size_bytes != 0) drive.partitions.push_back(std::move(partition));
        }
        drive.live_medium = DiskHasPartUuid(entry.path(), live_uuid);
        drive.unsafe_reason = UnsafeReason(drive);
        if (!mountinfo_valid || !swapinfo_valid || !live_uuid_valid) {
            drive.unsafe_reason = "Cannot identify the live source and mounted devices safely.";
        }
        drive.safe = drive.unsafe_reason.empty();
        drives.push_back(std::move(drive));
    }

    std::sort(drives.begin(), drives.end(), [](const Drive& a, const Drive& b) {
        if (a.safe != b.safe) return a.safe > b.safe;
        if (a.size_bytes != b.size_bytes) return a.size_bytes > b.size_bytes;
        return a.path < b.path;
    });
    return drives;
}

ValidationResult ValidateOperation(const OperationRequest& request,
                                   const std::vector<Drive>& current_drives, bool live_mode) {
    if (!live_mode) return {false, "Installer operations are available only in a live session.", {}};
    if (request.api_version != kOperationApiVersion)
        return {false, "Unsupported installer operation API version.", {}};
    if (request.target_disk_id.empty()) return {false, "A target drive must be selected.", {}};
    const auto it = std::find_if(current_drives.begin(), current_drives.end(),
                                 [&](const Drive& drive) { return drive.id == request.target_disk_id; });
    if (it == current_drives.end()) return {false, "The selected drive is no longer available.", {}};
    // Enumeration carries fail-closed errors (including unreadable mountinfo
    // or a missing live-medium UUID) that cannot be reconstructed here.
    if (!it->safe)
        return {false, it->unsafe_reason.empty() ? "Drive safety could not be established." : it->unsafe_reason,
                *it};
    if (const std::string reason = UnsafeReason(*it); !reason.empty())
        return {false, reason, *it};

    std::string error;
    const bool valid = std::visit([&](const auto& op) -> bool {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, WriteGpt>) return CheckGpt(op, *it, &error);
        else if constexpr (std::is_same_v<T, CreateLpMetadata>) {
            const auto super = std::find_if(it->partitions.begin(), it->partitions.end(), [&](const Partition& p) {
                return p.part_guid == op.super_part_guid && !p.logical;
            });
            if (super == it->partitions.end() || super->mounted || op.metadata_size_bytes == 0 ||
                op.metadata_slots < 2 || op.groups.empty()) { error = "Invalid LP metadata description or super target."; return false; }
            uint64_t group_sum = 0;
            for (const auto& group : op.groups) {
                uint64_t member_sum = 0;
                if (group.name.empty() || group.partitions.empty()) { error = "LP group is empty."; return false; }
                for (const auto& member : group.partitions) {
                    if (member.name.empty() || member.size_bytes == 0 || member_sum > UINT64_MAX - member.size_bytes) {
                        error = "Invalid or overflowing LP member."; return false;
                    }
                    member_sum += member.size_bytes;
                }
                if (member_sum > group.maximum_size_bytes || group_sum > UINT64_MAX - group.maximum_size_bytes) {
                    error = "LP group exceeds its declared capacity."; return false;
                }
                group_sum += group.maximum_size_bytes;
            }
            if (group_sum > super->size_bytes || op.metadata_size_bytes > super->size_bytes - group_sum) {
                error = "LP metadata and groups exceed the declared super partition."; return false;
            }
            return true;
        } else if constexpr (std::is_same_v<T, Format>) {
            if (!CheckRef(*it, op.target) || op.label.size() > 16) { error = "Invalid format target or label."; return false; }
            return true;
        } else if constexpr (std::is_same_v<T, CopyPartition>) {
            if (!CheckRef(*it, op.target) || (op.source_kind == CopySourceKind::kTargetPartition && !CheckRef(*it, op.source)) ||
                (op.source_kind == CopySourceKind::kLiveImagePartition && op.live_partition_name.empty())) {
                error = "Copy source or destination is unavailable or outside the target."; return false;
            }
            return true;
        } else if constexpr (std::is_same_v<T, ClonePartition>) {
            if (!CheckRef(*it, op.source) || !CheckRef(*it, op.target)) { error = "Clone reference is unavailable or outside the target."; return false; }
            const auto* src = FindPartition(*it, op.source); const auto* dst = FindPartition(*it, op.target);
            if (src->size_bytes > dst->size_bytes) { error = "Clone destination is smaller than source."; return false; }
            return true;
        } else if constexpr (std::is_same_v<T, WriteFiles>) {
            if (!CheckRef(*it, op.target) || !SafeRelative(op.relative_directory) || op.files.empty()) { error = "Invalid file write target or directory."; return false; }
            for (const auto& file : op.files) if (!SafeRelative(file.relative_path) ||
                (file.live_payload_path.empty() == file.inline_contents.empty())) { error = "Invalid file payload path or source."; return false; }
            return true;
        } else if constexpr (std::is_same_v<T, CopyUserFiles>) {
            if (!CheckRef(*it, op.target_userdata)) { error = "Invalid user-data target."; return false; }
            for (const auto& p : op.include_paths) if (!SafeRelative(p)) { error = "Unsafe include path."; return false; }
            for (const auto& p : op.exclude_paths) if (!SafeRelative(p)) { error = "Unsafe exclude path."; return false; }
            return true;
        }
        return false;
    }, request.operation);
    if (!valid) return {false, error.empty() ? "Invalid operation." : error, *it};
    return {true, "Request passed current drive safety checks.", *it};
}

ValidationResult ExecuteOne(const OperationRequest& request, const SnapshotProvider& snapshot,
                            const PrimitiveExecutor& executor, bool live_mode,
                            ProgressCallback progress, CancelCallback cancelled) {
    // Each call takes one fresh snapshot and performs no work until every guard passes.
    const auto current = snapshot ? snapshot() : std::vector<Drive>{};
    auto result = ValidateOperation(request, current, live_mode);
    if (!result.ok) return result;
    if (!executor || (cancelled && cancelled())) return {false, "Operation cancelled or executor unavailable.", result.target};
    std::string error;
    if (!executor(request.operation, result.target, std::move(progress), std::move(cancelled), &error))
        return {false, error.empty() ? "Primitive operation failed." : error, result.target};
    return {true, "Primitive operation completed.", result.target};
}

}  // namespace matonos::install
