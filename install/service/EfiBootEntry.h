#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace matonos::install {

// Fallback path on the ESP that the installer always writes (shim). The NVRAM
// entry points at the same file so Secure Boot keeps the shim -> MOK chain.
inline constexpr char kEfiLoaderPath[] = "\\EFI\\BOOT\\BOOTX64.EFI";
inline constexpr char kEfiEntryDescription[] = "MatonOS";

struct EfiHardDrive {
    uint32_t partition_number = 0;
    uint64_t start_lba = 0;
    uint64_t size_lba = 0;
    std::string part_guid;  // GPT unique partition GUID, textual
};

// EFI_LOAD_OPTION: active, HD(...)/File(path)/End, no optional data.
bool EncodeLoadOption(const EfiHardDrive& hd, const std::string& description, const std::string& path,
                      std::vector<uint8_t>* out, std::string* error);

// Creates or reuses a Boot#### variable for `hd` in `efivars_dir` and puts it
// first in BootOrder. Returns false with `error` set; callers treat a failure
// as non-fatal because the ESP fallback loader still boots.
bool RegisterBootEntry(const std::string& efivars_dir, const EfiHardDrive& hd, std::string* error);

}  // namespace matonos::install
