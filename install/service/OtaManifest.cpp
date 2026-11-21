#include "OtaManifest.h"

#include "Json.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sys/stat.h>

namespace fs = std::filesystem;
namespace matonos::install::ota {
namespace {
constexpr const char* kFiles[] = {
    "system.erofs", "system_ext.erofs", "product.erofs", "vendor.erofs",
    "odm.erofs", "vbmeta.img", "matonos-a.efi", "matonos-b.efi",
    "matonos-a-hdd.efi", "matonos-b-hdd.efi"};

bool Read(const fs::path& path, std::vector<uint8_t>* out, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { *error = "open failed: " + path.string(); return false; }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) { *error = "size query failed: " + path.string(); return false; }
    out->resize(static_cast<size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!out->empty() && !input.read(reinterpret_cast<char*>(out->data()), out->size())) {
        *error = "read failed: " + path.string(); return false;
    }
    return true;
}

bool Number(const json::Value* value, uint64_t* output) {
    if (!value || value->kind != json::Value::Kind::kNumber || value->scalar.empty()) return false;
    errno = 0; char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value->scalar.c_str(), &end, 10);
    if (errno || !end || *end || parsed == 0) return false;
    *output = static_cast<uint64_t>(parsed); return true;
}

std::string String(const json::Value* value) {
    return value && value->kind == json::Value::Kind::kString ? value->scalar : "";
}

std::string Hex(const uint8_t* bytes, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { result.push_back(digits[bytes[i] >> 4]); result.push_back(digits[bytes[i] & 15]); }
    return result;
}

bool Digest(const std::vector<uint8_t>& data, std::string* digest) {
    std::array<uint8_t, 32> bytes{}; unsigned int size = 0;
    if (EVP_Digest(data.data(), data.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1 || size != bytes.size()) return false;
    *digest = Hex(bytes.data(), bytes.size()); return true;
}
}  // namespace

bool Verify(const std::string& directory, const std::string& public_key,
            Manifest* manifest, std::string* error) {
    std::vector<uint8_t> json_bytes, signature;
    if (!Read(fs::path(directory) / "manifest.json", &json_bytes, error) ||
        !Read(fs::path(directory) / "manifest.sig", &signature, error)) return false;
    if (signature.size() != 64) { *error = "manifest.sig must be a raw 64-byte Ed25519 signature"; return false; }
    FILE* key_file = std::fopen(public_key.c_str(), "rb");
    if (!key_file) { *error = "open public key failed: " + public_key; return false; }
    EVP_PKEY* key = PEM_read_PUBKEY(key_file, nullptr, nullptr, nullptr);
    std::fclose(key_file);
    if (!key || EVP_PKEY_id(key) != EVP_PKEY_ED25519) { if (key) EVP_PKEY_free(key); *error = "public key is not Ed25519 PEM"; return false; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool valid = ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(ctx, signature.data(), signature.size(), json_bytes.data(), json_bytes.size()) == 1;
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!valid) { *error = "manifest Ed25519 signature verification failed"; return false; }

    json::Value root;
    const std::string raw(reinterpret_cast<const char*>(json_bytes.data()), json_bytes.size());
    if (!json::Parse(raw, &root) || root.kind != json::Value::Kind::kObject) { *error = "manifest JSON is malformed"; return false; }
    uint64_t format = 0;
    if (!Number(root.Get("format_version"), &format) || format != 1) { *error = "unsupported manifest format_version"; return false; }
    const json::Value* build = root.Get("build"); uint64_t build_id = 0;
    uint64_t android_base = 0;
    if (!build || build->kind != json::Value::Kind::kObject ||
        !Number(build->Get("id"), &build_id) || !Number(build->Get("android_base"), &android_base) ||
        String(build->Get("version")).empty()) {
        *error = "manifest build metadata is invalid"; return false;
    }
    const json::Value* files = root.Get("files");
    if (!files || files->kind != json::Value::Kind::kArray || files->array.size() != std::size(kFiles)) {
        *error = "manifest must list exactly the eight OTA payload files"; return false;
    }
    Manifest result; result.build_id = build_id; result.android_base = android_base;
    result.version = String(build->Get("version"));
    std::set<std::string> seen;
    for (const auto& entry : files->array) {
        if (entry.kind != json::Value::Kind::kObject) { *error = "manifest file entry is not an object"; return false; }
        File file; file.name = String(entry.Get("name")); file.sha256 = String(entry.Get("sha256"));
        if (!Number(entry.Get("size"), &file.size) || file.sha256.size() != 64 ||
            file.sha256.find_first_not_of("0123456789abcdef") != std::string::npos || !seen.insert(file.name).second) {
            *error = "manifest file entry has invalid name, size, or lowercase sha256"; return false;
        }
        bool allowed = false; for (const char* name : kFiles) if (file.name == name) allowed = true;
        if (!allowed) { *error = "unsupported manifest payload name: " + file.name; return false; }
        std::vector<uint8_t> data;
        if (!Read(fs::path(directory) / file.name, &data, error)) return false;
        if (data.size() != file.size) { *error = "size mismatch for " + file.name; return false; }
        std::string hash;
        if (!Digest(data, &hash) || hash != file.sha256) { *error = "SHA-256 mismatch for " + file.name; return false; }
        result.files.push_back(std::move(file));
    }
    for (const char* name : kFiles) if (!seen.count(name)) { *error = std::string("manifest omits ") + name; return false; }
    const json::Value* addons = root.Get("addons");
    if (addons && addons->kind != json::Value::Kind::kObject) { *error = "manifest add-on inventory is not an object"; return false; }
    if (addons) for (const auto& item : addons->object) {
        if (item.first.empty() || item.second.kind != json::Value::Kind::kArray) { *error = "manifest add-on inventory has an invalid entry"; return false; }
        result.addons.insert(item.first);
    }
    *manifest = std::move(result); return true;
}

bool RequireAddons(const Manifest& manifest, const std::set<std::string>& installed,
                   std::string* error) {
    std::vector<std::string> missing;
    for (const auto& id : installed) if (!manifest.addons.count(id)) missing.push_back(id);
    if (missing.empty()) return true;
    *error = "signed OTA manifest is missing installed add-on IDs:";
    for (const auto& id : missing) *error += " " + id;
    return false;
}
}  // namespace matonos::install::ota
