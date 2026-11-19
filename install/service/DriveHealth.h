#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace matonos::install {

// One shared reader for install (findings shown before erase) and repair
// (risk rating). Health never blocks anything; callers only display it.
struct HealthFinding {
    enum class Level { kWarning, kError };
    Level level = Level::kWarning;
    std::string message;
};
struct DriveHealth {
    bool known = false;      // false: the drive or its bridge does not report SMART
    std::string source;      // "nvme" or "ata"
    std::vector<HealthFinding> findings;
};

// Parsed inputs, kept separate from the ioctls so they are host-testable.
struct NvmeHealthLog {
    uint8_t critical_warning = 0;
    uint16_t temperature_kelvin = 0;
    uint8_t available_spare = 0;
    uint8_t spare_threshold = 0;
    uint8_t percent_used = 0;
    uint64_t media_errors = 0;  // low 64 bits of the 128-bit counter, saturated
};
struct AtaAttribute { uint8_t id = 0; uint8_t value = 0; uint64_t raw = 0; };
struct AtaSmart {
    bool status_known = false;
    bool status_failing = false;
    bool rotational = true;
    std::vector<AtaAttribute> attributes;
};

NvmeHealthLog ParseNvmeHealthLog(const uint8_t log[512]);
std::vector<AtaAttribute> ParseAtaAttributes(const uint8_t data[512]);
DriveHealth EvaluateNvme(const NvmeHealthLog& log);
DriveHealth EvaluateAta(const AtaSmart& smart);

// Reads health from an open whole-disk block fd (needs the helper's caps).
DriveHealth ReadDriveHealth(int fd, bool nvme, bool rotational);
std::string HealthToJson(const DriveHealth& health);

}  // namespace matonos::install
