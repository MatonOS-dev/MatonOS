// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace matonos_addons {
struct VerifiedZip {
    std::string manifest;
    std::vector<uint8_t> signature;
    std::map<std::string, std::vector<uint8_t>> files;
};

std::string Sha256Hex(const std::vector<uint8_t>& bytes);
bool VerifyManifestSignature(const std::string& manifest, const std::vector<uint8_t>& signature,
                             const std::string& public_key_path, std::string* error);
bool ReadAndVerifyZip(const std::string& zip_path, const std::string& public_key_path,
                      VerifiedZip* result, std::string* error);
}  // namespace matonos_addons
