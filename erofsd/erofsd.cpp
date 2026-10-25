// matonos-erofsd: mounts EROFS images embedded in Linux stub and runtime APK
// splits at /mnt/matonos/<owner uid>/<n>, where the owner is the package
// shipping the split (file-backed EROFS, fsoffset, no loop device).
// Callers are checked through PackageManager: the caller and the package owning
// the split must both be signed with the pinned stub certificate, so no other
// app can feed images to the kernel parser. Mounts are shared by every caller
// using the same split; a periodic sweep unmounts unused or replaced ones.
// The mount syscall itself is init's (domain.te forbids it to other domains):
// the daemon sets matonos.erofs.* properties and matonos-erofsd.rc mounts.

#include <sys/stat.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <android-base/file.h>
#include <private/android_filesystem_config.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <android/content/pm/IPackageManagerNative.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <org/matonos/erofs/BnErofsMounter.h>
#include <ziparchive/zip_archive.h>

using android::IBinder;
using android::IPCThreadState;
using android::sp;
using android::wp;
using android::binder::Status;
using android::content::pm::IPackageManagerNative;
using android::base::StringPrintf;
using android::base::unique_fd;

namespace {

constexpr char kMountRoot[] = "/mnt/matonos";
// Short source names for init: APK paths can exceed the 92-byte property limit.
constexpr char kSourceDir[] = "/mnt/matonos/.src";
constexpr char kSubsystemPackage[] = "org.matonos.linux";
constexpr uint32_t kErofsMagic = 0xE0F5E1E2;
constexpr int64_t kErofsSuperOffset = 1024;
constexpr auto kSweepPeriod = std::chrono::minutes(10);
// SHA-256 of the MatonOS Linux subsystem release certificate (public data).
const std::vector<uint8_t> kSubsystemCert = {
    0x45, 0x96, 0xa6, 0xaf, 0xf5, 0x8a, 0x0b, 0xed, 0x52, 0xa9, 0xb2, 0x5b, 0x87, 0xa6, 0xa8, 0x9e,
    0xc8, 0x49, 0x01, 0x89, 0x41, 0xc2, 0xfb, 0x1d, 0xd3, 0x60, 0x22, 0x6e, 0x46, 0x79, 0xff, 0x58};

Status error(int code, const std::string& message) {
    LOG(WARNING) << message;
    return Status::fromServiceSpecificError(code, message.c_str());
}

sp<IPackageManagerNative> packageManager() {
    return android::waitForService<IPackageManagerNative>(android::String16("package_native"));
}

// The calling UID's package name ("" if none or a shared UID).
std::string callerPackage(int uid) {
    std::vector<std::string> names;
    auto pm = packageManager();
    if (!pm || !pm->getNamesForUids({uid}, &names).isOk() || names.size() != 1) return "";
    if (names[0].find(':') != std::string::npos) return "";  // shared UID: never ours
    return names[0];
}

// UID of `package` for `userId`, -1 if it is not installed there.
int packageUid(const std::string& package, int userId) {
    int uid = -1;
    auto pm = packageManager();
    if (!pm || !pm->getPackageUid(package, 0, userId, &uid).isOk()) return -1;
    return uid;
}

bool signedWith(const std::string& package, const std::vector<uint8_t>& sha256) {
    bool result = false;
    auto pm = packageManager();
    return pm && pm->hasSha256SigningCertificate(package, sha256, &result).isOk() && result;
}

// Package owning `path` (already resolved), "" if it is not an installed APK:
// /data/app/[~~<x>/]<package>-<y>/<file>.apk. Only PackageManager creates these;
// package names never contain '-'.
std::string splitPackage(const std::string& path) {
    if (!android::base::StartsWith(path, "/data/app/") || !android::base::EndsWith(path, ".apk")) {
        return "";
    }
    auto parts = android::base::Split(path.substr(strlen("/data/app/")), "/");
    if (parts.size() == 3 && android::base::StartsWith(parts[0], "~~")) parts.erase(parts.begin());
    size_t dash = parts.size() == 2 ? parts[0].find('-') : std::string::npos;
    return dash == std::string::npos || dash == 0 ? "" : parts[0].substr(0, dash);
}

struct Mount {
    std::string path;
    std::string apk;
    std::set<IBinder*> tokens;
    bool idle = false;  // no live tokens at the last sweep
};

class Mounter : public org::matonos::erofs::BnErofsMounter, public IBinder::DeathRecipient {
  public:
    Mounter() {
        std::thread([this] {
            for (;;) {
                std::this_thread::sleep_for(kSweepPeriod);
                sweep();
            }
        }).detach();
    }

    Status pinStubCertificate(const std::vector<uint8_t>& sha256) override {
        int uid = IPCThreadState::self()->getCallingUid();
        if (sha256.size() != 32) return error(1, "pin: certificate digest must be SHA-256");
        if (callerPackage(uid) != kSubsystemPackage || !signedWith(kSubsystemPackage, kSubsystemCert)) {
            return error(2, StringPrintf("pin: uid %d is not the MatonOS Linux subsystem", uid));
        }
        // In memory only: the subsystem re-shares the stub certificate on every boot.
        std::lock_guard<std::mutex> lock(mPinLock);
        mPin = sha256;
        LOG(INFO) << "stub certificate pinned by uid " << uid;
        return Status::ok();
    }

    Status mountSplit(const std::string& apkPath, const std::string& entry, const sp<IBinder>& token,
                      std::string* out) override {
        int uid = IPCThreadState::self()->getCallingUid();
        if (!token) return error(1, "mount: missing lifetime token");
        if (!android::base::StartsWith(entry, "assets/matonos/") ||
            !android::base::EndsWith(entry, ".erofs") || entry.find("..") != std::string::npos) {
            return error(1, "mount: entry must be assets/matonos/*.erofs");
        }
        std::vector<uint8_t> pin;
        {
            std::lock_guard<std::mutex> lock(mPinLock);
            pin = mPin;
        }
        if (pin.empty()) return error(4, "mount: no stub certificate pinned");
        std::string package = callerPackage(uid);
        if (package.empty() || !signedWith(package, pin)) {
            return error(2, StringPrintf("mount: uid %d is not a pinned Linux stub", uid));
        }
        unique_fd fd(open(apkPath.c_str(), O_RDONLY | O_CLOEXEC));
        std::string real;
        if (fd < 0 || !android::base::Readlink(StringPrintf("/proc/self/fd/%d", fd.get()), &real)) {
            return error(2, StringPrintf("mount: cannot open %s", apkPath.c_str()));
        }
        std::string owner = splitPackage(real);
        if (owner.empty() || (owner != package && !signedWith(owner, pin))) {
            return error(2, StringPrintf("mount: %s is not an installed stub or runtime split",
                                         real.c_str()));
        }
        int64_t offset = 0;
        if (int e = imageOffset(fd.get(), real, entry, &offset)) {
            return error(5, StringPrintf("mount: %s!%s is not a stored, aligned EROFS image (%d)",
                                         real.c_str(), entry.c_str(), e));
        }

        std::lock_guard<std::mutex> lock(mLock);
        std::string key = real + "!" + entry;
        auto it = mMounts.find(key);
        if (it == mMounts.end()) {
            int ownerUid = owner == package ? uid : packageUid(owner, uid / AID_USER_OFFSET);
            if (ownerUid < 0) return error(2, StringPrintf("mount: %s is not installed", owner.c_str()));
            std::string ownerDir = StringPrintf("%s/%d", kMountRoot, ownerUid);
            if (mkdir(ownerDir.c_str(), 0755) && errno != EEXIST) {
                return error(6, StringPrintf("mount: mkdir %s: %s", ownerDir.c_str(), strerror(errno)));
            }
            std::string target = StringPrintf("%s/%u", ownerDir.c_str(), ++mCounter);
            if (mkdir(target.c_str(), 0755) && errno != EEXIST) {
                return error(6, StringPrintf("mount: mkdir %s: %s", target.c_str(), strerror(errno)));
            }
            if (!initMount(real, target, offset)) {
                rmdir(target.c_str());
                return error(6, StringPrintf("mount: init could not mount %s", key.c_str()));
            }
            LOG(INFO) << "mounted " << key << " at " << target << " for uid " << uid;
            it = mMounts.emplace(key, Mount{target, real, {}}).first;
        }
        it->second.idle = false;
        if (it->second.tokens.insert(token.get()).second) {
            token->linkToDeath(sp<IBinder::DeathRecipient>::fromExisting(this));
            mTokens[token.get()].insert(key);
        }
        *out = it->second.path;
        return Status::ok();
    }

    // Dead tokens only release their mounts; the sweep unmounts.
    void binderDied(const wp<IBinder>& who) override {
        IBinder* token = who.unsafe_get();
        std::lock_guard<std::mutex> lock(mLock);
        auto held = mTokens.find(token);
        if (held == mTokens.end()) return;
        for (const auto& key : held->second) {
            auto it = mMounts.find(key);
            if (it != mMounts.end()) it->second.tokens.erase(token);
        }
        mTokens.erase(held);
    }

  private:
    // Unmounts mounts whose APK is gone (package updated or removed) and mounts
    // that had no live tokens for a whole sweep period.
    void sweep() {
        std::lock_guard<std::mutex> lock(mLock);
        for (auto it = mMounts.begin(); it != mMounts.end();) {
            Mount& m = it->second;
            bool replaced = access(m.apk.c_str(), F_OK) != 0;
            if (!replaced && (!m.tokens.empty() || !m.idle)) {
                m.idle = m.tokens.empty();
                ++it;
                continue;
            }
            if (!initUnmount(m.path)) {
                LOG(WARNING) << "unmount " << m.path << " failed (busy?); retrying next sweep";
                ++it;
                continue;
            }
            LOG(INFO) << "unmounted " << m.path << (replaced ? " (apk replaced)" : " (unused)");
            for (IBinder* token : m.tokens) mTokens[token].erase(it->first);
            it = mMounts.erase(it);
        }
    }

    // Calls run one at a time (mLock held); each request id is new, so every
    // property write changes the value and fires its init trigger.
    bool initMount(const std::string& apk, const std::string& target, int64_t offset) {
        std::string id = std::to_string(++mRequest);
        std::string source = StringPrintf("%s/%s", kSourceDir, id.c_str());
        if (symlink(apk.c_str(), source.c_str())) {
            PLOG(WARNING) << "symlink " << source;
            return false;
        }
        android::base::SetProperty("matonos.erofs.target", target);
        android::base::SetProperty("matonos.erofs.offset", std::to_string(offset));
        android::base::SetProperty("matonos.erofs.mount", id);
        android::base::WaitForProperty("matonos.erofs.done", id);
        unlink(source.c_str());
        return isErofs(target);
    }

    bool initUnmount(const std::string& target) {
        std::string id = std::to_string(++mRequest);
        android::base::SetProperty("matonos.erofs.utarget", target);
        android::base::SetProperty("matonos.erofs.umount", id);
        android::base::WaitForProperty("matonos.erofs.done", id);
        if (isErofs(target)) return false;
        rmdir(target.c_str());
        return true;
    }

    // init reports completion, not success: check what is mounted there.
    static bool isErofs(const std::string& path) {
        struct statfs fs;
        return statfs(path.c_str(), &fs) == 0 && static_cast<uint32_t>(fs.f_type) == kErofsMagic;
    }

    // Payload offset of a stored, 4096-aligned entry carrying the EROFS magic.
    static int imageOffset(int fd, const std::string& name, const std::string& entry, int64_t* out) {
        ZipArchiveHandle zip;
        if (OpenArchiveFd(fd, name.c_str(), &zip, false)) return 1;
        ZipEntry64 found;
        int result = FindEntry(zip, entry, &found) ? 2
                     : found.method != kCompressStored ? 3
                     : found.offset % 4096 ? 4
                     : 0;
        uint32_t magic = 0;
        if (!result && (pread(fd, &magic, sizeof(magic), found.offset + kErofsSuperOffset) != sizeof(magic) ||
                        magic != kErofsMagic)) {
            result = 5;
        }
        if (!result) *out = found.offset;
        CloseArchive(zip);
        return result;
    }

    std::mutex mPinLock;
    std::vector<uint8_t> mPin;
    std::mutex mLock;
    unsigned mCounter = 0;
    unsigned mRequest = 0;
    std::map<std::string, Mount> mMounts;
    std::map<IBinder*, std::set<std::string>> mTokens;
};

}  // namespace

int main(int, char** argv) {
    android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));
    sp<Mounter> service = sp<Mounter>::make();
    if (android::defaultServiceManager()->addService(android::String16("matonos.erofs"), service) !=
        android::OK) {
        LOG(FATAL) << "cannot register matonos.erofs";
    }
    android::ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();
    return 0;
}
