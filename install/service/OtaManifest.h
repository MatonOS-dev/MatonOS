#pragma once

#include <cstdint>
#include <string>
#include <set>
#include <vector>

namespace matonos::install::ota {

struct File {
    std::string name;
    uint64_t size = 0;
    std::string sha256;
};

struct Manifest {
    uint64_t build_id = 0;
    uint64_t android_base = 0;
    std::string version;
    std::vector<File> files;
    std::set<std::string> addons;
};

bool RequireAddons(const Manifest& manifest, const std::set<std::string>& installed,
                   std::string* error);

// Verify detached Ed25519 signature, schema, exact payload inventory, sizes,
// and hashes. Both the on-device writer and host tests use this function.
bool Verify(const std::string& directory, const std::string& public_key,
            Manifest* manifest, std::string* error);

}  // namespace matonos::install::ota
