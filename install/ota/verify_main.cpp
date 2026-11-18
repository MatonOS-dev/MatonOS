#include "../service/OtaManifest.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) { std::fprintf(stderr, "usage: ota-manifest-verify <directory> <public-key.pem> [installed-android-base]\n"); return 2; }
    matonos::install::ota::Manifest manifest; std::string error;
    if (!matonos::install::ota::Verify(argv[1], argv[2], &manifest, &error)) { std::fprintf(stderr, "verify failed: %s\n", error.c_str()); return 1; }
    if (argc == 4) {
        char* end = nullptr; const unsigned long long minimum = std::strtoull(argv[3], &end, 10);
        if (!end || *end || minimum == 0 || manifest.android_base < minimum) { std::fprintf(stderr, "verify failed: OTA Android base is lower\n"); return 1; }
    }
    std::printf("OK build_id=%llu android_base=%llu version=%s\n", static_cast<unsigned long long>(manifest.build_id), static_cast<unsigned long long>(manifest.android_base), manifest.version.c_str());
    return 0;
}
