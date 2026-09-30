// SPDX-License-Identifier: Apache-2.0
#include "package_zip.h"

#include "monocypher.h"
#include <zlib.h>

#include <algorithm>
#include <fstream>

namespace matonos_addons {
namespace {
constexpr size_t kMaxZip = 128U * 1024U * 1024U;
constexpr size_t kMaxMember = 64U * 1024U * 1024U;
constexpr size_t kMaxExpanded = 120U * 1024U * 1024U;
constexpr size_t kMaxEntries = 512;

uint16_t U16(const std::vector<uint8_t>& b, size_t p) {
    return static_cast<uint16_t>(b[p]) | static_cast<uint16_t>(b[p + 1] << 8);
}
uint32_t U32(const std::vector<uint8_t>& b, size_t p) {
    return static_cast<uint32_t>(b[p]) | (static_cast<uint32_t>(b[p + 1]) << 8) |
           (static_cast<uint32_t>(b[p + 2]) << 16) | (static_cast<uint32_t>(b[p + 3]) << 24);
}
bool InRange(size_t size, uint64_t off, uint64_t len) {
    return off <= size && len <= size - off;
}
bool SafePath(const std::string& path) {
    if (path.empty() || path.size() > 255 || path.front() == '/' || path.back() == '/') return false;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+'))
                return false;
        }
        start = end + 1;
    }
    return true;
}
bool ReadBounded(const std::string& path, size_t limit, std::vector<uint8_t>* bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > limit) return false;
    bytes->resize(static_cast<size_t>(end));
    file.seekg(0);
    return bytes->empty() || static_cast<bool>(file.read(reinterpret_cast<char*>(bytes->data()), end));
}
bool InflateMember(const std::vector<uint8_t>& zip, uint16_t method, size_t data_offset,
                   uint32_t compressed_size, uint32_t expanded_size,
                   std::vector<uint8_t>* output) {
    if (expanded_size > kMaxMember || !InRange(zip.size(), data_offset, compressed_size)) return false;
    output->resize(expanded_size);
    if (method == 0) {
        if (compressed_size != expanded_size) return false;
        std::copy_n(zip.data() + data_offset, expanded_size, output->data());
        return true;
    }
    if (method != 8) return false;
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(zip.data() + data_offset);
    stream.avail_in = compressed_size;
    stream.next_out = output->data();
    stream.avail_out = expanded_size;
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
    const int rc = inflate(&stream, Z_FINISH);
    const bool ok = rc == Z_STREAM_END && stream.total_out == expanded_size &&
                    stream.total_in == compressed_size;
    inflateEnd(&stream);
    return ok;
}
struct CentralEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed = 0;
    uint32_t expanded = 0;
    uint32_t local_offset = 0;
};
}  // namespace

bool VerifyManifestSignature(const std::string& manifest, const std::vector<uint8_t>& signature,
                             const std::string& public_key_path, std::string* error) {
    if (manifest.empty() || manifest.size() > 1024U * 1024U || signature.size() != 64) {
        *error = "manifest or detached signature has an invalid size";
        return false;
    }
    std::vector<uint8_t> public_key;
    if (!ReadBounded(public_key_path, 32, &public_key) || public_key.size() != 32) {
        *error = "trusted 32-byte repository key is unavailable";
        return false;
    }
    if (crypto_eddsa_check(signature.data(), public_key.data(),
                           reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()) != 0) {
        *error = "repository signature verification failed";
        return false;
    }
    return true;
}

bool ReadAndVerifyZip(const std::string& zip_path, const std::string& public_key_path,
                      VerifiedZip* result, std::string* error) {
    std::vector<uint8_t> zip;
    if (!ReadBounded(zip_path, kMaxZip, &zip) || zip.size() < 22) {
        *error = "ZIP missing, empty, or over 128 MiB";
        return false;
    }
    const size_t search_begin = zip.size() > 65557 ? zip.size() - 65557 : 0;
    size_t eocd = std::string::npos;
    for (size_t p = zip.size() - 22;; --p) {
        if (U32(zip, p) == 0x06054b50U && InRange(zip.size(), p, 22 + U16(zip, p + 20)) &&
            p + 22 + U16(zip, p + 20) == zip.size()) {
            eocd = p;
            break;
        }
        if (p == search_begin) break;
    }
    if (eocd == std::string::npos || U16(zip, eocd + 4) || U16(zip, eocd + 6) ||
        U16(zip, eocd + 8) != U16(zip, eocd + 10)) {
        *error = "unsupported, multi-disk, or malformed ZIP directory";
        return false;
    }
    const uint16_t count = U16(zip, eocd + 10);
    const uint32_t central_size = U32(zip, eocd + 12);
    const uint32_t central_offset = U32(zip, eocd + 16);
    if (!count || count > kMaxEntries || count == 0xffff || central_size == 0xffffffffU ||
        central_offset == 0xffffffffU || !InRange(zip.size(), central_offset, central_size) ||
        central_offset + central_size > eocd) {
        *error = "ZIP64 or oversized central directory is not supported";
        return false;
    }
    std::vector<CentralEntry> entries;
    size_t pos = central_offset;
    for (uint16_t i = 0; i < count; ++i) {
        if (!InRange(zip.size(), pos, 46) || U32(zip, pos) != 0x02014b50U) {
            *error = "invalid ZIP central directory entry";
            return false;
        }
        const uint16_t flags = U16(zip, pos + 8), method = U16(zip, pos + 10);
        const uint32_t compressed = U32(zip, pos + 20), expanded = U32(zip, pos + 24);
        const uint16_t name_len = U16(zip, pos + 28), extra_len = U16(zip, pos + 30),
                       comment_len = U16(zip, pos + 32);
        const uint16_t disk = U16(zip, pos + 34);
        const uint32_t local = U32(zip, pos + 42);
        const uint64_t entry_len = 46ULL + name_len + extra_len + comment_len;
        if (!InRange(zip.size(), pos, entry_len) || !name_len || flags != 0 || disk ||
            compressed == 0xffffffffU || expanded == 0xffffffffU || local == 0xffffffffU ||
            expanded > kMaxMember || (method != 0 && method != 8)) {
            *error = "unsupported ZIP feature, encryption, or oversized member";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(zip.data() + pos + 46), name_len);
        if (!SafePath(name) || std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return e.name == name; })) {
            *error = "unsafe or duplicate ZIP member path";
            return false;
        }
        entries.push_back({std::move(name), method, U32(zip, pos + 16), compressed, expanded, local});
        pos += static_cast<size_t>(entry_len);
    }
    if (pos != static_cast<size_t>(central_offset) + central_size) {
        *error = "central directory length does not match its entries";
        return false;
    }
    size_t total_expanded = 0;
    std::vector<uint8_t> signature;
    for (const auto& entry : entries) {
        if (!InRange(zip.size(), entry.local_offset, 30) || U32(zip, entry.local_offset) != 0x04034b50U) {
            *error = "invalid ZIP local header";
            return false;
        }
        const uint16_t flags = U16(zip, entry.local_offset + 6), method = U16(zip, entry.local_offset + 8);
        const uint16_t name_len = U16(zip, entry.local_offset + 26), extra_len = U16(zip, entry.local_offset + 28);
        const size_t data_offset = static_cast<size_t>(entry.local_offset) + 30 + name_len + extra_len;
        if (flags != 0 || method != entry.method || name_len != entry.name.size() ||
            !InRange(zip.size(), entry.local_offset + 30, static_cast<uint64_t>(name_len) + extra_len) ||
            std::string(reinterpret_cast<const char*>(zip.data() + entry.local_offset + 30), name_len) != entry.name ||
            !InRange(zip.size(), data_offset, entry.compressed)) {
            *error = "ZIP local and central headers disagree";
            return false;
        }
        std::vector<uint8_t> data;
        if (!InflateMember(zip, entry.method, data_offset, entry.compressed, entry.expanded, &data) ||
            crc32(0, data.data(), static_cast<uInt>(data.size())) != entry.crc) {
            *error = "member decompression or CRC check failed";
            return false;
        }
        total_expanded += data.size();
        if (total_expanded > kMaxExpanded) {
            *error = "expanded ZIP exceeds 120 MiB";
            return false;
        }
        if (entry.name == "manifest.json") result->manifest.assign(data.begin(), data.end());
        else if (entry.name == "manifest.sig") signature = std::move(data);
        else result->files.emplace(entry.name, std::move(data));
    }
    if (result->manifest.empty() || signature.size() != 64) {
        *error = "package must contain manifest.json and a 64-byte manifest.sig";
        return false;
    }
    result->signature = signature;
    if (!VerifyManifestSignature(result->manifest, result->signature, public_key_path, error)) return false;
    return true;
}
}  // namespace matonos_addons
