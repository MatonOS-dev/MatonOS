#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace matonos::install {

// Wire API v1 is deliberately a primitive executor. The app supplies policy,
// layout, and operation ordering; callers submit one operation at a time.
inline constexpr uint32_t kOperationApiVersion = 1;

struct Partition {
    std::string name;
    std::string path;  // service-resolved node, never accepted from callers
    std::string part_guid;
    std::string type_guid;
    std::string backing_part_guid;
    uint64_t start_bytes = 0;
    uint64_t size_bytes = 0;
    bool mounted = false;
    bool in_use = false;
    bool logical = false;
};

struct Drive {
    std::string id;  // current kernel major:minor locator, not authorization identity
    std::string identity;  // canonical sysfs path + disk sequence + normalized WWID/serial
    std::string physical_id;  // normalized WWID, else serial; empty when unavailable
    uint64_t disk_sequence = 0;  // kernel diskseq, rechecked on the opened block device
    std::string path;  // display only; never accepted as an operation operand
    std::string model;
    std::string serial;
    std::string transport;
    uint64_t size_bytes = 0;
    bool removable = false;
    bool read_only = false;
    bool mounted = false;
    bool in_use = false;
    bool live_medium = false;
    bool safe = false;
    std::string unsafe_reason;
    std::vector<Partition> partitions;
};

struct GptPartitionSpec {
    std::string name;
    std::string type_guid;
    std::string part_guid;
    uint64_t start_bytes = 0;
    uint64_t size_bytes = 0;
};
struct WriteGpt { uint64_t declared_disk_bytes = 0; std::string disk_guid; std::vector<GptPartitionSpec> partitions; };
struct LogicalPartitionSpec { std::string name; uint64_t size_bytes = 0; };
struct LogicalGroupSpec { std::string name; uint64_t maximum_size_bytes = 0; std::vector<LogicalPartitionSpec> partitions; };
struct CreateLpMetadata {
    std::string super_part_guid;
    uint64_t metadata_size_bytes = 0;
    uint32_t metadata_slots = 0;
    std::vector<LogicalGroupSpec> groups;
};
struct PartitionRef { std::string part_guid; std::string logical_name; };
enum class Filesystem { kVfat, kExt4, kF2fs };
struct Format { PartitionRef target; Filesystem filesystem; std::string label; };
enum class CopySourceKind { kLiveImagePartition, kTargetPartition };
struct CopyPartition { CopySourceKind source_kind; std::string live_partition_name; PartitionRef source; PartitionRef target; };
struct ClonePartition { PartitionRef source; PartitionRef target; };
struct FilePayload { std::string relative_path; std::string live_payload_path; std::string inline_contents; std::string sha256; };
struct WriteFiles { PartitionRef target; std::string relative_directory; std::vector<FilePayload> files; };
using Operation = std::variant<WriteGpt, CreateLpMetadata, Format, CopyPartition,
                               ClonePartition, WriteFiles>;
struct OperationRequest {
    uint32_t api_version = kOperationApiVersion;
    std::string target_disk_id;  // current locator
    std::string target_disk_identity;  // captured at selection and required for every primitive
    Operation operation;
};
struct ValidationResult { bool ok = false; std::string message; Drive target; };
struct OperationProgress { uint64_t bytes_done = 0; uint64_t bytes_total = 0; std::string message; };
using ProgressCallback = std::function<void(const OperationProgress&)>;
using CancelCallback = std::function<bool()>;
using SnapshotProvider = std::function<std::vector<Drive>()>;
using PrimitiveExecutor = std::function<bool(const Operation&, const Drive&, ProgressCallback, CancelCallback, std::string*)>;
// Built-in execution backend used only by the live-gated installer daemon.
bool ExecutePrimitive(const Operation& operation, const Drive& target,
                      ProgressCallback progress, CancelCallback cancelled,
                      std::string* error);

std::vector<Drive> EnumerateDrives(const std::string& sys_block_path,
                                   const std::string& dev_block_path,
                                   const std::string& mountinfo,
                                   const std::string& cmdline,
                                   const std::string& swapinfo = "/proc/swaps");
ValidationResult ValidateOperation(const OperationRequest& request,
                                   const std::vector<Drive>& fresh_drives,
                                   bool live_mode);
// Fetches a new enumeration and validates immediately before dispatch. This is
// the only intended destructive dispatch path; the backend must not cache fd/path.
ValidationResult ExecuteOne(const OperationRequest& request, const SnapshotProvider& snapshot,
                            const PrimitiveExecutor& executor, bool live_mode,
                            ProgressCallback progress, CancelCallback cancelled);

}  // namespace matonos::install
