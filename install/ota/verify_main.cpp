#include "../service/OtaManifest.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) { std::fprintf(stderr, "usage: ota-manifest-verify <directory> <public-key.pem> [installed-build-id]\n"); return 2; }
    matonos::install::ota::Manifest manifest; std::string error;
    if (!matonos::install::ota::Verify(argv[1], argv[2], &manifest, &error)) { std::fprintf(stderr, "verify failed: %s\n", error.c_str()); return 1; }
    if (argc == 4) {
        char* end = nullptr; const unsigned long long minimum = std::strtoull(argv[3], &end, 10);
        if (!end || *end || minimum == 0 || manifest.build_id <= minimum) { std::fprintf(stderr, "verify failed: OTA build id is not newer\n"); return 1; }
    }
    std::printf("OK channel=%s build_id=%llu version=%s\n", manifest.channel.c_str(), static_cast<unsigned long long>(manifest.build_id), manifest.version.c_str());
    return 0;
}
