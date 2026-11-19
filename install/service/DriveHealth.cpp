#include "DriveHealth.h"

#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#include <cstring>
#include <sstream>

namespace matonos::install {
namespace {

constexpr unsigned kTimeoutMs = 5000;  // a hung USB bridge must not stall the drive list

void Add(DriveHealth* h, HealthFinding::Level level, const std::string& message) {
    h->findings.push_back({level, message});
}
constexpr auto kError = HealthFinding::Level::kError;
constexpr auto kWarning = HealthFinding::Level::kWarning;

const AtaAttribute* FindAttribute(const AtaSmart& smart, uint8_t id) {
    for (const auto& a : smart.attributes) if (a.id == id) return &a;
    return nullptr;
}

std::string Plural(uint64_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

void WearFinding(DriveHealth* h, unsigned used) {
    if (used > 100) Add(h, kError, "This SSD has used up its rated write cycles (" + std::to_string(used) + "%).");
    else if (used >= 80) Add(h, kWarning, "This SSD has used about " + std::to_string(used) + "% of its write cycles.");
}

bool AtaPassThrough(int fd, uint8_t feature, bool data_in, uint8_t data[512], uint8_t sense[32]) {
    uint8_t cdb[16] = {};
    cdb[0] = 0x85;                        // ATA PASS-THROUGH (16)
    cdb[1] = data_in ? (4 << 1) : (3 << 1);  // PIO data-in / non-data
    cdb[2] = data_in ? 0x0e : 0x20;       // data-in: T_DIR|BYT_BLOK|T_LENGTH=count; non-data: CK_COND
    cdb[4] = feature;
    cdb[6] = 1;
    cdb[10] = 0x4f;                       // SMART signature
    cdb[12] = 0xc2;
    cdb[14] = 0xb0;                       // SMART
    sg_io_hdr_t io{};
    io.interface_id = 'S';
    io.cmd_len = sizeof(cdb);
    io.cmdp = cdb;
    io.mx_sb_len = 32;
    io.sbp = sense;
    io.dxfer_direction = data_in ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
    io.dxfer_len = data_in ? 512 : 0;
    io.dxferp = data_in ? data : nullptr;
    io.timeout = kTimeoutMs;
    std::memset(sense, 0, 32);
    if (ioctl(fd, SG_IO, &io) != 0) return false;
    if (data_in) return (io.info & SG_INFO_OK_MASK) == SG_INFO_OK;
    return true;  // CK_COND reports the ATA registers through sense data
}

}  // namespace

NvmeHealthLog ParseNvmeHealthLog(const uint8_t log[512]) {
    NvmeHealthLog out;
    out.critical_warning = log[0];
    out.temperature_kelvin = static_cast<uint16_t>(log[1] | log[2] << 8);
    out.available_spare = log[3];
    out.spare_threshold = log[4];
    out.percent_used = log[5];
    uint64_t low = 0;
    for (int i = 0; i < 8; ++i) low |= static_cast<uint64_t>(log[160 + i]) << (8 * i);
    bool high = false;
    for (int i = 8; i < 16; ++i) high |= log[160 + i] != 0;
    out.media_errors = high ? UINT64_MAX : low;
    return out;
}

std::vector<AtaAttribute> ParseAtaAttributes(const uint8_t data[512]) {
    std::vector<AtaAttribute> out;
    for (int i = 0; i < 30; ++i) {
        const uint8_t* a = data + 2 + i * 12;
        if (a[0] == 0) continue;
        uint64_t raw = 0;
        for (int b = 0; b < 6; ++b) raw |= static_cast<uint64_t>(a[5 + b]) << (8 * b);
        out.push_back({a[0], a[3], raw});
    }
    return out;
}

DriveHealth EvaluateNvme(const NvmeHealthLog& log) {
    DriveHealth h;
    h.known = true;
    h.source = "nvme";
    if (log.critical_warning & 0x04) Add(&h, kError, "This drive reports SMART errors: its reliability is degraded.");
    if (log.critical_warning & 0x08) Add(&h, kError, "This drive reports SMART errors: it has switched itself to read-only.");
    if (log.critical_warning & 0x10) Add(&h, kError, "This drive reports SMART errors: its power-loss backup has failed.");
    if ((log.critical_warning & 0x01) || (log.spare_threshold && log.available_spare < log.spare_threshold))
        Add(&h, kError, "This drive's spare capacity (" + std::to_string(log.available_spare) +
                            "%) is below its own safety threshold.");
    if (log.media_errors) Add(&h, kError, "This drive reports " + Plural(log.media_errors, "media error", "media errors") + ".");
    WearFinding(&h, log.percent_used);
    if (log.critical_warning & 0x02) {
        const int celsius = static_cast<int>(log.temperature_kelvin) - 273;
        Add(&h, kWarning, "This drive is overheating (" + std::to_string(celsius) + " °C).");
    }
    return h;
}

DriveHealth EvaluateAta(const AtaSmart& smart) {
    DriveHealth h;
    h.known = smart.status_known || !smart.attributes.empty();
    h.source = "ata";
    if (!h.known) return h;
    if (smart.status_failing) Add(&h, kError, "This drive reports SMART errors: its own health check fails.");
    // Low 32 bits: several vendors pack extra data into the high raw bytes.
    const auto count = [&](uint8_t id) -> uint64_t { const auto* a = FindAttribute(smart, id); return a ? a->raw & 0xffffffffu : 0; };
    if (const uint64_t n = count(197)) Add(&h, kError, "This drive reports " + Plural(n, "unreadable sector", "unreadable sectors") + " waiting to be remapped.");
    if (const uint64_t n = count(198)) Add(&h, kError, "This drive reports " + Plural(n, "uncorrectable sector", "uncorrectable sectors") + ".");
    if (const uint64_t n = count(187)) Add(&h, kError, "This drive reports " + Plural(n, "uncorrectable read error", "uncorrectable read errors") + ".");
    if (const uint64_t n = count(5)) Add(&h, kWarning, "This drive reports " + Plural(n, "bad sector", "bad sectors") + " (already remapped).");
    if (!smart.rotational) {
        // Normalized value = remaining life (100 = new) on the common SSD wear attributes.
        for (const uint8_t id : {177, 231, 233}) {
            if (const auto* a = FindAttribute(smart, id); a && a->value <= 100) { WearFinding(&h, 100u - a->value); break; }
        }
    }
    if (const auto* t = FindAttribute(smart, 194); t && (t->raw & 0xff) >= 70 && (t->raw & 0xff) < 128)
        Add(&h, kWarning, "This drive is overheating (" + std::to_string(t->raw & 0xff) + " °C).");
    return h;
}

DriveHealth ReadDriveHealth(int fd, bool nvme, bool rotational) {
    if (nvme) {
        uint8_t log[512] = {};
        nvme_admin_cmd cmd{};
        cmd.opcode = 0x02;              // Get Log Page
        cmd.nsid = 0xffffffff;
        cmd.addr = reinterpret_cast<uintptr_t>(log);
        cmd.data_len = sizeof(log);
        cmd.cdw10 = (127u << 16) | 0x02;  // 128 dwords of SMART / Health Information
        cmd.timeout_ms = kTimeoutMs;
        if (ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) != 0) return {false, "nvme", {}};
        return EvaluateNvme(ParseNvmeHealthLog(log));
    }
    AtaSmart smart;
    smart.rotational = rotational;
    uint8_t data[512] = {}, sense[32];
    if (AtaPassThrough(fd, 0xda, false, data, sense) && sense[0] == 0x72 && sense[8] == 0x09) {
        // ATA status return descriptor: LBA mid/high at bytes 9 and 11.
        const uint8_t mid = sense[8 + 9], high = sense[8 + 11];
        if (mid == 0x4f && high == 0xc2) smart.status_known = true;
        else if (mid == 0xf4 && high == 0x2c) smart.status_known = smart.status_failing = true;
    }
    if (AtaPassThrough(fd, 0xd0, true, data, sense)) smart.attributes = ParseAtaAttributes(data);
    return EvaluateAta(smart);
}

std::string HealthToJson(const DriveHealth& health) {
    const auto escape = [](const std::string& s) {
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
        }
        return out + "\"";
    };
    std::ostringstream out;
    out << "{\"known\":" << (health.known ? "true" : "false") << ",\"source\":" << escape(health.source) << ",\"findings\":[";
    for (size_t i = 0; i < health.findings.size(); ++i) {
        if (i) out << ',';
        out << "{\"level\":\"" << (health.findings[i].level == HealthFinding::Level::kError ? "error" : "warning")
            << "\",\"message\":" << escape(health.findings[i].message) << '}';
    }
    out << "]}";
    return out.str();
}

}  // namespace matonos::install
