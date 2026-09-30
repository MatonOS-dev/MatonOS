#include "InstallerService.h"
#include "LpMetadata.h"

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

std::string NormalizeDeviceId(std::string value) {
    std::string normalized;
    for (unsigned char c : value) {
        if (!std::isspace(c)) normalized.push_back(static_cast<char>(std::tolower(c)));
    }
    return normalized;
}

std::string DeviceIdentity(const std::string& sys_path, uint64_t disk_sequence,
                           const std::string& wwid, const std::string& serial) {
    if (sys_path.empty() || disk_sequence == 0) return {};
    // diskseq distinguishes a replacement even when Linux reuses both the
    // block number and physical bus path. WWID/serial strengthen the binding
    // across aliases and make the selected device visible to the caller.
    return "sys=" + sys_path + ";seq=" + std::to_string(disk_sequence) +
           ";wwid=" + NormalizeDeviceId(wwid) + ";serial=" + NormalizeDeviceId(serial);
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
    if (!part || part->mounted || part->size_bytes == 0) return false;
    if (part->logical) {
        const auto super=std::find_if(drive.partitions.begin(),drive.partitions.end(),[&](const Partition& p){return !p.logical&&p.part_guid==part->backing_part_guid;});
        return super!=drive.partitions.end()&&part->start_bytes<=super->size_bytes&&part->size_bytes<=super->size_bytes-part->start_bytes;
    }
    return part->start_bytes <= drive.size_bytes && part->size_bytes <= drive.size_bytes - part->start_bytes;
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
            p.start_bytes < 1024 * 1024 || p.start_bytes % (1024 * 1024) != 0 || p.size_bytes % 512 != 0 || p.start_bytes > disk.size_bytes ||
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
        drive.serial = Read(entry.path() / "device" / "serial");

        std::error_code real_error;
        const std::string real_sys = fs::canonical(entry.path(), real_error).string();
        drive.disk_sequence = std::strtoull(Read(entry.path() / "diskseq").c_str(), nullptr, 10);
        std::string wwid = Read(entry.path() / "wwid");
        if (wwid.empty()) wwid = Read(entry.path() / "device" / "wwid");
        drive.identity = DeviceIdentity(real_sys, drive.disk_sequence, wwid, drive.serial);
        const std::string normalized_wwid = NormalizeDeviceId(wwid);
        const std::string normalized_serial = NormalizeDeviceId(drive.serial);
        if (!normalized_wwid.empty()) drive.physical_id = "wwid=" + normalized_wwid;
        else if (!normalized_serial.empty()) drive.physical_id = "serial=" + normalized_serial;
        drive.transport = Transport(real_sys, name);
        const auto physical_ids = PhysicalDeviceIds(entry.path());
        drive.mounted = std::any_of(physical_ids.begin(), physical_ids.end(),
                                    [&](const std::string& id) { return mounted.count(id) != 0; });
        drive.in_use = HasHolders(entry.path());
        for (const auto& child : fs::directory_iterator(entry.path(), ec)) {
            if (ec) break;
            if (!fs::exists(child.path() / "partition", ec)) continue;
            Partition partition;
            partition.path=(fs::path(dev_block_path)/child.path().filename()).string();
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
        // Read LP metadata from an unmounted physical super partition. The
        // executor addresses its validated linear extents directly through
        // that partition, so it does not need to create temporary dm devices.
        auto super_it=std::find_if(drive.partitions.begin(),drive.partitions.end(),[](const Partition& p){return !p.logical&&p.name=="super"&&!p.path.empty();});
        if(super_it!=drive.partitions.end()) {
            const Partition physical=*super_it;
            std::vector<Partition> logical;
            if(ReadLpLogicalPartitions(physical.path,physical.part_guid,physical.name,physical.size_bytes,&logical)) {
                for(auto& p:logical) {
                    // Mapper names are global. During a live install, a
                    // same-named system_a on the live disk must not make the
                    // unmounted target's system_a look mounted. The mapper
                    // ancestry pass below associates mounted state only when
                    // its backing device is this physical disk.
                    drive.partitions.push_back(std::move(p));
                }
            }
        }
        // Also expose mapped logical partitions, but only when their device
        // mapper ancestry resolves to this disk. This prevents a similarly
        // named logical partition on the live medium being mistaken for the
        // selected target's member.
        std::error_code mapper_error;
        const fs::path mapper_dir = fs::path(dev_block_path) / "mapper";
        for (const auto& mapped : fs::directory_iterator(mapper_dir, mapper_error)) {
            if (mapper_error) break;
            struct stat mapped_stat {};
            if (::stat(mapped.path().c_str(), &mapped_stat) != 0 || !S_ISBLK(mapped_stat.st_mode)) continue;
            const std::string mapped_id = DevId(major(mapped_stat.st_rdev), minor(mapped_stat.st_rdev));
            const fs::path sys_device = fs::canonical(fs::path(sys_block_path).parent_path() / "dev" / "block" / mapped_id, mapper_error);
            if (mapper_error) { mapper_error.clear(); continue; }
            bool belongs_to_disk = false;
            std::error_code slaves_error;
            const fs::path slaves = sys_device / "slaves";
            for (const auto& slave : fs::directory_iterator(slaves, slaves_error)) {
                if (slaves_error) break;
                unsigned smaj=0,smin=0;
                if (!ParseDev(Read(slave.path()/"dev"),&smaj,&smin)) continue;
                const auto slave_ids=PhysicalDeviceIds(fs::canonical(slave.path(),slaves_error));
                for (const auto& physical_id : physical_ids) if (slave_ids.count(physical_id)) belongs_to_disk=true;
            }
            if (!belongs_to_disk) continue;
            Partition logical;
            logical.name=mapped.path().filename().string();
            logical.logical=true;
            logical.mounted=mounted.count(mapped_id)!=0;
            logical.size_bytes=std::strtoull(Read(sys_device/"size").c_str(),nullptr,10)*512ULL;
            if(logical.size_bytes) drive.partitions.push_back(std::move(logical));
        }
        drive.live_medium = DiskHasPartUuid(entry.path(), live_uuid);
        drive.unsafe_reason = UnsafeReason(drive);
        if (drive.identity.empty()) drive.unsafe_reason = "A unique physical disk identity could not be established.";
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
    if (request.target_disk_identity.empty() || it->identity != request.target_disk_identity)
        return {false, "The selected physical drive changed. Refresh the drive list and confirm the target again.", *it};
    if (std::count_if(current_drives.begin(), current_drives.end(), [&](const Drive& drive) {
            return drive.identity == request.target_disk_identity;
        }) != 1)
        return {false, "The selected physical drive identity is ambiguous.", *it};
    if (!it->physical_id.empty() && std::count_if(current_drives.begin(), current_drives.end(), [&](const Drive& drive) {
            return drive.physical_id == it->physical_id;
        }) != 1)
        return {false, "Multiple disks report the same WWID or serial; refresh and select an unambiguous target.", *it};
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
            uint64_t allocated_sum = 0;
            for (const auto& group : op.groups) {
                uint64_t member_sum = 0;
                if (group.name.empty() || group.partitions.empty()) { error = "LP group is empty."; return false; }
                for (const auto& member : group.partitions) {
                    if (member.name.empty() || member_sum > UINT64_MAX - member.size_bytes) {
                        error = "Invalid or overflowing LP member."; return false;
                    }
                    member_sum += member.size_bytes;
                }
                if (member_sum > group.maximum_size_bytes || allocated_sum > UINT64_MAX - member_sum) {
                    error = "LP group exceeds its declared capacity."; return false;
                }
                allocated_sum += member_sum;
            }
            // A/B group capacities describe future OTA targets and can each
            // exceed the physical super when only slot A is allocated.
            if (allocated_sum > super->size_bytes || op.metadata_size_bytes > super->size_bytes - allocated_sum) {
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
