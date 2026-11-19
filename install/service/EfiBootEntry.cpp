#include "EfiBootEntry.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace matonos::install {
namespace {

constexpr char kGlobalGuid[] = "8be4df61-93ca-11d2-aa0d-00e098032b8c";
constexpr uint32_t kAttributes = 0x7;  // NON_VOLATILE | BOOTSERVICE | RUNTIME
constexpr uint32_t kLoadOptionActive = 0x1;

void Put16(std::vector<uint8_t>* out, uint16_t v) { out->push_back(v & 0xff); out->push_back(v >> 8); }
void Put32(std::vector<uint8_t>* out, uint32_t v) { for (int i = 0; i < 4; ++i) out->push_back((v >> (8 * i)) & 0xff); }
void Put64(std::vector<uint8_t>* out, uint64_t v) { for (int i = 0; i < 8; ++i) out->push_back((v >> (8 * i)) & 0xff); }

void PutUtf16(std::vector<uint8_t>* out, const std::string& ascii) {
    for (const char c : ascii) Put16(out, static_cast<uint8_t>(c));
    Put16(out, 0);
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// EFI_GUID byte order: first three fields little-endian, the rest as written.
bool ParseGuid(const std::string& text, uint8_t out[16]) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') return false;
    uint8_t raw[16];
    size_t n = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        const int hi = Hex(text[i]), lo = Hex(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        raw[n++] = static_cast<uint8_t>(hi << 4 | lo);
        ++i;
    }
    static constexpr int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) out[i] = raw[kOrder[i]];
    return true;
}

bool ReadVariable(const std::string& path, std::vector<uint8_t>* data, int* error_number) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { *error_number = errno; return false; }
    std::vector<uint8_t> all;
    uint8_t buffer[4096];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { *error_number = errno; close(fd); return false; }
        if (n == 0) break;
        all.insert(all.end(), buffer, buffer + n);
    }
    close(fd);
    if (all.size() < 4) { *error_number = EIO; return false; }
    data->assign(all.begin() + 4, all.end());
    return true;
}

// efivarfs takes attributes + data in a single write.
bool WriteVariable(const std::string& path, const std::vector<uint8_t>& data, std::string* error) {
    std::vector<uint8_t> payload;
    Put32(&payload, kAttributes);
    payload.insert(payload.end(), data.begin(), data.end());
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { *error = "open " + path + ": " + std::strerror(errno); return false; }
    ssize_t n;
    do n = write(fd, payload.data(), payload.size()); while (n < 0 && errno == EINTR);
    const int write_errno = errno;
    close(fd);
    if (n != static_cast<ssize_t>(payload.size())) {
        *error = "write " + path + ": " + (n < 0 ? std::strerror(write_errno) : "short write");
        return false;
    }
    return true;
}

std::string BootName(uint16_t number) {
    char name[16];
    std::snprintf(name, sizeof(name), "Boot%04X", number);
    return name;
}

}  // namespace

bool EncodeLoadOption(const EfiHardDrive& hd, const std::string& description, const std::string& path,
                      std::vector<uint8_t>* out, std::string* error) {
    uint8_t signature[16];
    if (!ParseGuid(hd.part_guid, signature)) { *error = "invalid ESP partition GUID " + hd.part_guid; return false; }
    if (hd.partition_number == 0 || hd.size_lba == 0) { *error = "invalid ESP partition geometry"; return false; }
    std::vector<uint8_t> device_path;
    // MEDIA_DEVICE_PATH / MEDIA_HARDDRIVE_DP, 42 bytes.
    device_path.push_back(0x04); device_path.push_back(0x01); Put16(&device_path, 42);
    Put32(&device_path, hd.partition_number);
    Put64(&device_path, hd.start_lba);
    Put64(&device_path, hd.size_lba);
    device_path.insert(device_path.end(), signature, signature + 16);
    device_path.push_back(0x02);  // MBRType: GPT
    device_path.push_back(0x02);  // SignatureType: GUID
    // MEDIA_DEVICE_PATH / MEDIA_FILEPATH_DP.
    const size_t file_length = 4 + 2 * (path.size() + 1);
    if (file_length > UINT16_MAX) { *error = "loader path too long"; return false; }
    device_path.push_back(0x04); device_path.push_back(0x04); Put16(&device_path, static_cast<uint16_t>(file_length));
    PutUtf16(&device_path, path);
    // END_DEVICE_PATH_TYPE / END_ENTIRE_DEVICE_PATH_SUBTYPE.
    device_path.push_back(0x7f); device_path.push_back(0xff); Put16(&device_path, 4);

    out->clear();
    Put32(out, kLoadOptionActive);
    Put16(out, static_cast<uint16_t>(device_path.size()));
    PutUtf16(out, description);
    out->insert(out->end(), device_path.begin(), device_path.end());
    return true;
}

bool RegisterBootEntry(const std::string& efivars_dir, const EfiHardDrive& hd, std::string* error) {
    std::vector<uint8_t> option;
    if (!EncodeLoadOption(hd, kEfiEntryDescription, kEfiLoaderPath, &option, error)) return false;
    const std::string suffix = std::string("-") + kGlobalGuid;

    // Reuse an identical entry (reinstall onto the same layout); else take the lowest free number.
    std::vector<bool> used(0x10000, false);
    int number = -1;
    DIR* dir = opendir(efivars_dir.c_str());
    if (!dir) { *error = "open " + efivars_dir + ": " + std::strerror(errno); return false; }
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() != 8 + suffix.size() || name.compare(0, 4, "Boot") != 0 || name.compare(8, std::string::npos, suffix) != 0)
            continue;
        int value = 0;
        bool hex = true;
        for (size_t i = 4; i < 8; ++i) {
            const int digit = Hex(name[i]);
            if (digit < 0 || (name[i] >= 'a' && name[i] <= 'f')) { hex = false; break; }
            value = value << 4 | digit;
        }
        if (!hex) continue;
        used[value] = true;
        std::vector<uint8_t> existing;
        int read_errno = 0;
        if (number < 0 && ReadVariable(efivars_dir + "/" + name, &existing, &read_errno) && existing == option) number = value;
    }
    closedir(dir);
    if (number < 0) {
        const auto free_slot = std::find(used.begin(), used.end(), false);
        if (free_slot == used.end()) { *error = "no free Boot#### variable"; return false; }
        number = static_cast<int>(free_slot - used.begin());
        if (!WriteVariable(efivars_dir + "/" + BootName(number) + suffix, option, error)) return false;
    }

    std::vector<uint8_t> order;
    int read_errno = 0;
    const std::string order_path = efivars_dir + "/BootOrder" + suffix;
    if (!ReadVariable(order_path, &order, &read_errno)) {
        if (read_errno != ENOENT) { *error = "read BootOrder: " + std::string(std::strerror(read_errno)); return false; }
        order.clear();
    }
    std::vector<uint8_t> updated;
    Put16(&updated, static_cast<uint16_t>(number));
    for (size_t i = 0; i + 1 < order.size(); i += 2) {
        const uint16_t value = order[i] | order[i + 1] << 8;
        if (value != number) Put16(&updated, value);
    }
    if (updated == order) return true;
    return WriteVariable(order_path, updated, error);
}

}  // namespace matonos::install
