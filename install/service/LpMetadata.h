#pragma once

#include "InstallerService.h"

#include <string>
#include <vector>

namespace matonos::install {

bool WriteLpMetadata(const std::string& super_path, uint64_t super_size_bytes,
                     const std::string& super_partition_name,
                     const CreateLpMetadata& request, uint64_t disk_sequence,
                     std::string* error);
bool ReadLpLogicalPartitions(const std::string& super_path,
                             const std::string& super_part_guid,
                             const std::string& super_partition_name,
                             uint64_t super_size_bytes,
                             std::vector<Partition>* partitions);

}  // namespace matonos::install
