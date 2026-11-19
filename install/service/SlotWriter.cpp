#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/partition_opener.h>
#include <openssl/evp.h>

#include "service/FatFilesystem.h"
#include "service/OtaManifest.h"

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mi = matonos::install;
namespace {
constexpr char kMetadata[] = "/metadata/maton/images";
constexpr char kImageDir[] = "/maton_system/images";
constexpr char kEsp[] = "/dev/block/by-name/esp";
constexpr char kOtaStageRoot[] = "/maton_system/ota-staging";
constexpr char kImages[][16] = {"system", "system_ext", "product", "vendor", "odm"};
constexpr size_t kCopyBytes = 1024 * 1024;

struct Slot {
    char letter;
    int index;
    char upper;
};

bool GetSlots(Slot* running, Slot* target) {
    char suffix[PROP_VALUE_MAX]{};
    if (__system_property_get("ro.boot.slot_suffix", suffix) <= 0) {
        std::fprintf(stderr,"matonos-slot-writer: ro.boot.slot_suffix is unavailable\n");
        return false;
    }
    if (std::strcmp(suffix,"_a")==0) *running={'a',0,'A'};
    else if (std::strcmp(suffix,"_b")==0) *running={'b',1,'B'};
    else {
        std::fprintf(stderr,"matonos-slot-writer: unsupported running slot suffix '%s'\n",suffix);
        return false;
    }
    *target = running->index == 0 ? Slot{'b',1,'B'} : Slot{'a',0,'A'};
    return true;
}

std::string EntryPath(const Slot& slot, bool counted) {
    return std::string("loader/entries/") + slot.upper + (counted ? "+3-0" : "") + ".conf";
}

// A staged slot's entry stays disabled (not a .conf, so systemd-boot ignores it)
// until BootControl activates it with ordering fields and a boot counter.
std::string StagedEntryPath(const Slot& slot) {
    return std::string("loader/entries/") + slot.upper + ".DIS";
}

std::string UkiPath(const Slot& slot) {
    return std::string("EFI/Linux/matonos-") + slot.letter + ".efi";
}

std::string ImageName(const char* base, const Slot& slot) {
    return std::string(base) + "_" + slot.letter;
}

#define CLOSE_FD(fd_value) do { const int n=(fd_value); if(n>=0){std::fprintf(stderr,"matonos-slot-writer: close fd=%d at %s:%d\n",n,__FILE__,__LINE__);if(::close(n)!=0)std::fprintf(stderr,"matonos-slot-writer: close fd=%d failed errno=%d at %s:%d\n",n,errno,__FILE__,__LINE__);} } while(0)

bool Fail(const std::string& context) {
    std::fprintf(stderr, "matonos-slot-writer: %s (errno=%d: %s)\n", context.c_str(), errno, strerror(errno));
    return false;
}

bool ReadRegular(const fs::path& path, int* fd, uint64_t* size) {
    *fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (*fd < 0) return Fail("open payload " + path.string());
    struct stat st{};
    if (fstat(*fd, &st) != 0) { CLOSE_FD(*fd); *fd=-1; return Fail("stat payload " + path.string()); }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) { CLOSE_FD(*fd); *fd=-1; return Fail("payload is not a nonempty regular file: " + path.string()); }
    *size = static_cast<uint64_t>(st.st_size);
    return true;
}

// ImageManager::MapImageDevice records each mapping in a gsid.mapped_image.*
// property, which only gsid, init and update_engine may set. Map through
// device-mapper directly: the dm device is named after the image, so unmapping
// needs no record and also removes devices first-stage init mapped.
bool UnmapImage(const std::string& name) {
    return android::dm::DeviceMapper::Instance().DeleteDeviceIfExists(name);
}

bool MapImage(const std::string& name, android::fiemap::ImageManager* manager, std::string* path) {
    android::fs_mgr::PartitionOpener opener;
    std::string device;
    if (!manager->MapImageWithDeviceMapper(opener, name, &device)) return false;
    if (!android::dm::DeviceMapper::Instance().GetDmDevicePathByName(name, path)) {
        UnmapImage(name);
        return false;
    }
    // ueventd creates the node asynchronously.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (access(path->c_str(), F_OK) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) { UnmapImage(name); errno = ETIMEDOUT; return false; }
        usleep(50 * 1000);
    }
    return true;
}

bool UnmapInactive(const std::string& name) {
    if (!UnmapImage(name)) return Fail("remove dm device for inactive image " + name);
    return true;
}

bool CopyToImage(int source, const std::string& name, uint64_t size,
                 android::fiemap::ImageManager* manager) {
    if (size % 512 != 0) return Fail("payload size is not 512-byte aligned for " + name);
    if (!UnmapInactive(name)) return false;
    if (manager->BackingImageExists(name) && !manager->DeleteBackingImage(name))
        return Fail("remove previous inactive image " + name);
    if (!manager->CreateBackingImage(name, size, android::fiemap::IImageManager::CREATE_IMAGE_READONLY, nullptr))
        return Fail("create libfiemap image and extent metadata for " + name);
    std::string block;
    if (!MapImage(name, manager, &block))
        return Fail("map new image " + name);
    int output = open(block.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (output < 0) {
        const int e=errno;
        if(!UnmapImage(name))std::fprintf(stderr,"matonos-slot-writer: failed to unmap %s after open failure\n",name.c_str());
        errno=e;return Fail("open mapped image " + block);
    }
    std::vector<uint8_t> bytes(kCopyBytes);
    uint64_t offset=0;
    while(offset<size) {
        const size_t want=static_cast<size_t>(std::min<uint64_t>(bytes.size(),size-offset));
        ssize_t got;
        do { got=pread(source,bytes.data(),want,static_cast<off_t>(offset)); } while(got<0&&errno==EINTR);
        if(got<=0){const int e=got<0?errno:EIO;CLOSE_FD(output);if(!UnmapImage(name))std::fprintf(stderr,"matonos-slot-writer: failed to unmap %s after read failure\n",name.c_str());errno=e;return Fail("read payload for " + name);}
        size_t done=0;
        while(done<static_cast<size_t>(got)) {
            ssize_t put;
            do { put=pwrite(output,bytes.data()+done,static_cast<size_t>(got)-done,static_cast<off_t>(offset+done)); } while(put<0&&errno==EINTR);
            if(put<=0){const int e=put<0?errno:EIO;CLOSE_FD(output);if(!UnmapImage(name))std::fprintf(stderr,"matonos-slot-writer: failed to unmap %s after write failure\n",name.c_str());errno=e;return Fail("write mapped image " + name);}
            done+=static_cast<size_t>(put);
        }
        offset+=static_cast<uint64_t>(got);
    }
    if(fsync(output)!=0){const int e=errno;CLOSE_FD(output);if(!UnmapImage(name))std::fprintf(stderr,"matonos-slot-writer: failed to unmap %s after fsync failure\n",name.c_str());errno=e;return Fail("fsync mapped image " + name);}
    CLOSE_FD(output);
    if(!UnmapImage(name)) return Fail("unmap completed image " + name);
    return true;
}

bool OpenEsp(bool writable, mi::FatVolume* volume, std::string* error) {
    std::error_code ec;
    const auto device=fs::canonical(kEsp,ec);
    if(ec){*error="Cannot resolve ESP block device: "+ec.message();return false;}
    return mi::FatVolume::Open(device.string(), writable, volume, error);
}

uint32_t U32(const uint8_t* p) {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}

uint64_t U64(const uint8_t* p) {
    return (uint64_t(U32(p))<<32)|U32(p+4);
}

bool VbmetaDigest(int fd, uint64_t image_size, std::string* digest) {
    uint8_t header[256]{};
    if (image_size<sizeof(header)) { errno=EINVAL; return Fail("vbmeta image is shorter than its AVB header"); }
    ssize_t header_read;
    do { header_read=pread(fd,header,sizeof(header),0); } while(header_read<0&&errno==EINTR);
    if (header_read!=static_cast<ssize_t>(sizeof(header))) {
        if(header_read==0)errno=EIO;
        return Fail("read vbmeta AVB header");
    }
    if (std::memcmp(header,"AVB0",4)!=0) { errno=EINVAL; return Fail("vbmeta image has no AVB0 header"); }
    const uint64_t auth_size=U64(header+12), aux_size=U64(header+20);
    if (auth_size>UINT64_MAX-sizeof(header) || aux_size>UINT64_MAX-sizeof(header)-auth_size ||
        sizeof(header)+auth_size+aux_size>image_size) {
        errno=EINVAL; return Fail("vbmeta header block sizes exceed the image");
    }
    const uint64_t total=sizeof(header)+auth_size+aux_size;
    EVP_MD_CTX* context=EVP_MD_CTX_new();
    if(!context)return Fail("allocate SHA-256 context for vbmeta digest");
    bool ok=EVP_DigestInit_ex(context,EVP_sha256(),nullptr)==1;
    std::vector<uint8_t> buffer(kCopyBytes);
    uint64_t offset=0;
    while(ok&&offset<total) {
        const size_t want=static_cast<size_t>(std::min<uint64_t>(buffer.size(),total-offset));
        ssize_t got;
        do { got=pread(fd,buffer.data(),want,static_cast<off_t>(offset)); } while(got<0&&errno==EINTR);
        if(got<=0) { if(got==0)errno=EIO; ok=false; break; }
        ok=EVP_DigestUpdate(context,buffer.data(),static_cast<size_t>(got))==1;
        offset+=static_cast<uint64_t>(got);
    }
    uint8_t output[EVP_MAX_MD_SIZE]{}; unsigned int output_size=0;
    if(ok)ok=EVP_DigestFinal_ex(context,output,&output_size)==1;
    EVP_MD_CTX_free(context);
    if(!ok)return Fail("calculate SHA-256 vbmeta digest");
    if(output_size!=32){errno=EINVAL;return Fail("SHA-256 returned an unexpected digest size");}
    static constexpr char hex[]="0123456789abcdef";
    digest->clear();digest->reserve(64);
    for(unsigned int i=0;i<output_size;++i){digest->push_back(hex[output[i]>>4]);digest->push_back(hex[output[i]&15]);}
    return true;
}

bool UkiVbmetaDigest(const std::vector<uint8_t>& image, std::string* digest) {
    auto range_ok=[&](uint64_t offset,uint64_t size){return offset<=image.size()&&size<=image.size()-offset;};
    if(!range_ok(0x3c,4)){errno=EINVAL;return Fail("UKI lacks a DOS PE header pointer");}
    // DOS headers use little-endian fields, unlike AVB's big-endian metadata.
    const uint32_t pe_offset=uint32_t(image[0x3c])|(uint32_t(image[0x3d])<<8)|
        (uint32_t(image[0x3e])<<16)|(uint32_t(image[0x3f])<<24);
    if(!range_ok(pe_offset,24)||std::memcmp(image.data()+pe_offset,"PE\0\0",4)!=0){errno=EINVAL;return Fail("UKI has an invalid PE signature");}
    const uint16_t section_count=uint16_t(image[pe_offset+6])|(uint16_t(image[pe_offset+7])<<8);
    const uint16_t optional_size=uint16_t(image[pe_offset+20])|(uint16_t(image[pe_offset+21])<<8);
    const uint64_t sections=uint64_t(pe_offset)+24+optional_size;
    if(!range_ok(sections,uint64_t(section_count)*40)){errno=EINVAL;return Fail("UKI PE section table is truncated");}
    for(uint16_t i=0;i<section_count;++i) {
        const uint8_t* section=image.data()+sections+uint64_t(i)*40;
        if(std::memcmp(section,".cmdline",8)!=0)continue;
        const uint32_t raw_size=uint32_t(section[16])|(uint32_t(section[17])<<8)|
            (uint32_t(section[18])<<16)|(uint32_t(section[19])<<24);
        const uint32_t raw_offset=uint32_t(section[20])|(uint32_t(section[21])<<8)|
            (uint32_t(section[22])<<16)|(uint32_t(section[23])<<24);
        if(!range_ok(raw_offset,raw_size)){errno=EINVAL;return Fail("UKI .cmdline section exceeds the file");}
        std::string command(reinterpret_cast<const char*>(image.data()+raw_offset),raw_size);
        for(char& c:command)if(c=='\0')c=' ';
        size_t pos=0;bool found=false;
        while(pos<command.size()) {
            while(pos<command.size()&&std::isspace(static_cast<unsigned char>(command[pos])))++pos;
            size_t end=pos;while(end<command.size()&&!std::isspace(static_cast<unsigned char>(command[end])))++end;
            const std::string token=command.substr(pos,end-pos);
            constexpr char key[]="androidboot.vbmeta.digest=";
            if(token.rfind(key,0)==0) {
                const std::string value=token.substr(sizeof(key)-1);
                if(found||value.size()!=64||!std::all_of(value.begin(),value.end(),[](unsigned char c){return std::isxdigit(c)!=0;})){
                    errno=EINVAL;return Fail("UKI has a missing, duplicate, or malformed SHA-256 vbmeta digest");
                }
                *digest=value;std::transform(digest->begin(),digest->end(),digest->begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});found=true;
            }
            pos=end+1;
        }
        if(!found){errno=EINVAL;return Fail("UKI .cmdline has no androidboot.vbmeta.digest");}
        return true;
    }
    errno=EINVAL;return Fail("UKI has no .cmdline PE section");
}

bool RemoveEspIfPresent(mi::FatVolume* esp,const std::string& path,std::string* error) {
    return esp->RemoveFile(path,error);
}

// Every entry form BootControl may leave for a slot: counted, plain, disabled.
bool RemoveSlotEntries(mi::FatVolume* esp,const Slot& slot,std::string* error) {
    for(int left=0;left<=3;++left)for(int done=0;done<=3;++done)
        if(!RemoveEspIfPresent(esp,std::string("loader/entries/")+slot.upper+"+"+std::to_string(left)+"-"+std::to_string(done)+".conf",error))return false;
    return RemoveEspIfPresent(esp,EntryPath(slot,false),error)&&RemoveEspIfPresent(esp,StagedEntryPath(slot),error);
}

bool Stage(const fs::path& payload) {
    Slot running{},target{};
    if(!GetSlots(&running,&target))return false;
    mi::ota::Manifest manifest; std::string verify_error;
    if(!mi::ota::Verify(payload.string(), "/system_ext/etc/security/matonos/ota-ed25519.pub", &manifest, &verify_error)) {
        std::fprintf(stderr,"matonos-slot-writer: OTA manifest verification failed: %s\n",verify_error.c_str());return false;
    }
    // Installed add-ons live under vendor's /mnt/vendor, which Treble keeps from
    // system processes; the OTA server enforces the add-on gate (missing_addons).
    char release[PROP_VALUE_MAX]{};
    if(__system_property_get("ro.build.version.release",release)<=0) { errno=ENOENT; return Fail("ro.build.version.release is unavailable"); }
    char* base_end=nullptr; errno=0; const unsigned long long installed_base=std::strtoull(release,&base_end,10);
    if(errno||base_end==release||installed_base==0) { errno=EINVAL; return Fail("ro.build.version.release has no positive major version"); }
    if(manifest.android_base<installed_base) { errno=EINVAL; return Fail("refusing OTA with lower Android base version"); }
    if (access("/maton_system/images", R_OK|W_OK|X_OK)!=0 || access(kMetadata,R_OK|W_OK|X_OK)!=0)
        return Fail("installed image store or libfiemap metadata is unavailable");
    mi::FatVolume esp_read;std::string preflight_error;
    if(!OpenEsp(false,&esp_read,&preflight_error)){std::fprintf(stderr,"matonos-slot-writer: preflight ESP: %s\n",preflight_error.c_str());return false;}
    std::vector<uint8_t> a_bytes;
    if(!esp_read.ReadFile(EntryPath(running,false),&a_bytes,&preflight_error)){std::fprintf(stderr,"matonos-slot-writer: preflight running slot entry: %s\n",preflight_error.c_str());return false;}

    // Validate the supplied UKI/vbmeta pair before invalidating a previously staged slot.
    int vbmeta=-1;uint64_t vbmeta_size=0;
    if(!ReadRegular(payload/"vbmeta.img",&vbmeta,&vbmeta_size))return false;
    std::string vbmeta_digest;
    if(!VbmetaDigest(vbmeta,vbmeta_size,&vbmeta_digest)){CLOSE_FD(vbmeta);return false;}
    int uki=-1;uint64_t uki_size=0;
    const fs::path payload_uki=payload/(std::string("matonos-")+target.letter+".efi");
    if(!ReadRegular(payload_uki,&uki,&uki_size)){CLOSE_FD(vbmeta);return false;}
    std::vector<uint8_t> uki_bytes(static_cast<size_t>(uki_size));
    size_t read=0;
    while(read<uki_bytes.size()) { ssize_t n=pread(uki,uki_bytes.data()+read,uki_bytes.size()-read,read); if(n<0&&errno==EINTR)continue; if(n<=0){CLOSE_FD(uki);CLOSE_FD(vbmeta);return Fail("read target UKI");} read+=static_cast<size_t>(n); }
    CLOSE_FD(uki);
    std::string uki_digest;
    if(!UkiVbmetaDigest(uki_bytes,&uki_digest)){CLOSE_FD(vbmeta);return false;}
    CLOSE_FD(vbmeta);
    if(uki_digest!=vbmeta_digest){errno=EINVAL;return Fail("target UKI pinned vbmeta digest does not match payload vbmeta image");}

    mi::FatVolume esp;std::string error;
    if(!OpenEsp(true,&esp,&error)){std::fprintf(stderr,"matonos-slot-writer: open ESP for stale-slot cleanup: %s\n",error.c_str());return false;}
    if(!RemoveSlotEntries(&esp,target,&error)||
       !RemoveEspIfPresent(&esp,UkiPath(target),&error)||!esp.Sync(&error)){
        std::fprintf(stderr,"matonos-slot-writer: invalidate stale target entry/UKI: %s\n",error.c_str());return false;
    }

    auto manager=android::fiemap::ImageManager::Open(kMetadata,kImageDir);
    if(!manager)return Fail("open libfiemap image manager");
    for(const char* base:kImages) {
        const fs::path path=payload/(std::string(base)+".erofs");
        int fd=-1;uint64_t size=0;
        if(!ReadRegular(path,&fd,&size))return false;
        const bool copied=CopyToImage(fd,ImageName(base,target),size,manager.get());
        CLOSE_FD(fd);
        if(!copied)return false;
    }
    if(!ReadRegular(payload/"vbmeta.img",&vbmeta,&vbmeta_size))return false;
    const bool vbmeta_ok=CopyToImage(vbmeta,ImageName("vbmeta",target),vbmeta_size,manager.get());
    CLOSE_FD(vbmeta);
    if(!vbmeta_ok)return false;

    if(!esp.WriteFile(UkiPath(target),uki_bytes,&error)||!esp.Sync(&error)){std::fprintf(stderr,"matonos-slot-writer: write/sync target UKI: %s\n",error.c_str());return false;}
    // Entries carry no options: each slot's signed UKI embeds its own cmdline,
    // and first stage finds the ESP through the boot loader's EFI variables.
    const std::string updated=std::string("title MatonOS (slot ")+target.upper+")\nefi /"+UkiPath(target)+"\n";
    const std::string target_entry=StagedEntryPath(target);
    if(!esp.WriteFile(target_entry,std::vector<uint8_t>(updated.begin(),updated.end()),&error)||!esp.Sync(&error)){
        std::fprintf(stderr,"matonos-slot-writer: write/sync target staged entry: %s\n",error.c_str());return false;
    }
    std::fprintf(stderr,"matonos-slot-writer: staged slot %c images, vbmeta, UKI, and staged entry; slot %c remains running\n",target.upper,running.upper);
    return true;
}

bool RunBootctl(const char* command, const char* slot) {
    pid_t pid=fork();
    if(pid<0)return Fail("fork bootctl");
    if(pid==0){execl("/system/bin/bootctl","bootctl",command,slot,static_cast<char*>(nullptr));_exit(127);}
    int status=0;while(waitpid(pid,&status,0)<0){if(errno==EINTR)continue;return Fail("wait for bootctl");}
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)return Fail(std::string("bootctl ")+command+" failed for slot "+slot);
    return true;
}

bool Finalize() {
    Slot running{},target{};
    if(!GetSlots(&running,&target))return false;
    auto manager=android::fiemap::ImageManager::Open(kMetadata,kImageDir);
    if(!manager){return Fail("open libfiemap image manager before finalize");}
    for(const char* base:kImages){
        const std::string name=ImageName(base,target);
        if(!manager->BackingImageExists(name)){std::fprintf(stderr,"matonos-slot-writer: cannot finalize; missing target image %s\n",name.c_str());return false;}
    }
    const std::string vbmeta_name=ImageName("vbmeta",target);
    if(!manager->BackingImageExists(vbmeta_name)){std::fprintf(stderr,"matonos-slot-writer: cannot finalize; missing %s\n",vbmeta_name.c_str());return false;}
    std::string block;
    if(!UnmapInactive(vbmeta_name))return false;
    if(!MapImage(vbmeta_name, manager.get(), &block))return Fail("map target vbmeta for digest verification");
    int vbmeta_fd=open(block.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(vbmeta_fd<0){const int e=errno;if(!UnmapImage(vbmeta_name))std::fprintf(stderr,"matonos-slot-writer: failed to unmap %s after open failure\n",vbmeta_name.c_str());errno=e;return Fail("open mapped target vbmeta for digest verification");}
    uint64_t vbmeta_size=0;
    if(ioctl(vbmeta_fd,BLKGETSIZE64,&vbmeta_size)!=0||vbmeta_size==0){const int e=errno?errno:EINVAL;CLOSE_FD(vbmeta_fd);if(!UnmapImage(vbmeta_name))std::fprintf(stderr,"matonos-slot-writer: failed to unmap %s after size query failure\n",vbmeta_name.c_str());errno=e;return Fail("get mapped target vbmeta size");}
    std::string vbmeta_digest;
    const bool hashed=VbmetaDigest(vbmeta_fd,vbmeta_size,&vbmeta_digest);
    CLOSE_FD(vbmeta_fd);
    if(!UnmapImage(vbmeta_name))return Fail("unmap target vbmeta after digest verification");
    if(!hashed)return false;
    mi::FatVolume esp;std::string error;std::vector<uint8_t> contents;
    if(!OpenEsp(false,&esp,&error)||!esp.ReadFile(UkiPath(target),&contents,&error)||contents.empty()){
        std::fprintf(stderr,"matonos-slot-writer: cannot finalize; target UKI unavailable: %s\n",error.c_str());return false;
    }
    std::string uki_digest;
    if(!UkiVbmetaDigest(contents,&uki_digest))return false;
    if(uki_digest!=vbmeta_digest){errno=EINVAL;return Fail("target UKI pinned vbmeta digest does not match staged vbmeta image; slot was not switched");}
    if(!esp.ReadFile(StagedEntryPath(target),&contents,&error)||contents.empty()){
        std::fprintf(stderr,"matonos-slot-writer: cannot finalize; target staged entry unavailable: %s\n",error.c_str());return false;
    }
    const std::string slot_index=std::to_string(target.index);
    if(!RunBootctl("set-active-boot-slot",slot_index.c_str()))return false;
    std::fprintf(stderr,"matonos-slot-writer: BootControl selected slot %c\n",target.upper);
    return true;
}

bool RemoveInactive() {
    Slot running{},target{};
    if(!GetSlots(&running,&target))return false;
    const std::string running_index=std::to_string(running.index);
    if(!RunBootctl("is-slot-marked-successful",running_index.c_str())){std::fprintf(stderr,"matonos-slot-writer: refusing to remove inactive slot until running slot %c is marked successful\n",running.upper);return false;}
    mi::FatVolume esp;std::string error;
    if(!OpenEsp(true,&esp,&error)){std::fprintf(stderr,"matonos-slot-writer: open ESP for inactive-slot removal: %s\n",error.c_str());return false;}
    if(!RemoveSlotEntries(&esp,target,&error)||
       !RemoveEspIfPresent(&esp,UkiPath(target),&error)||!esp.Sync(&error)){
        std::fprintf(stderr,"matonos-slot-writer: remove inactive slot loader files: %s\n",error.c_str());return false;
    }
    auto manager=android::fiemap::ImageManager::Open(kMetadata,kImageDir);
    if(!manager)return Fail("open libfiemap image manager for inactive-slot removal");
    for(const char* base:kImages){const std::string name=ImageName(base,target);if(!UnmapInactive(name))return false;if(manager->BackingImageExists(name)&&!manager->DeleteBackingImage(name))return Fail("delete inactive image "+name);}
    const std::string vbmeta=ImageName("vbmeta",target);
    if(!UnmapInactive(vbmeta))return false;
    if(manager->BackingImageExists(vbmeta)&&!manager->DeleteBackingImage(vbmeta))return Fail("delete inactive vbmeta "+vbmeta);
    std::fprintf(stderr,"matonos-slot-writer: explicitly removed inactive slot %c; running slot %c is successful\n",target.upper,running.upper);return true;
}

bool WriteResult(const std::string& path,const std::string& id,bool ok,const std::string& output) {
    const std::string temporary=path+"."+id+".tmp";
    const std::string result=id+"\n"+(ok?"0":"1")+"\n"+output+"\n";
    const int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return Fail("create slot-writer result file "+temporary);
    size_t written=0;
    while(written<result.size()) {
        const ssize_t n=write(fd,result.data()+written,result.size()-written);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){const int e=n<0?errno:EIO;CLOSE_FD(fd);errno=e;return Fail("write slot-writer result file "+temporary);}
        written+=static_cast<size_t>(n);
    }
    if(fsync(fd)!=0){const int e=errno;CLOSE_FD(fd);errno=e;return Fail("sync slot-writer result file "+temporary);}
    CLOSE_FD(fd);
    if(rename(temporary.c_str(),path.c_str())!=0)return Fail("publish slot-writer result file "+path);
    return true;
}

bool RunRequest(const char* request_path,const char* result_path) {
    std::ifstream request(request_path);
    std::string id,operation,payload,extra;
    if(!std::getline(request,id)||!std::getline(request,operation)||!std::getline(request,payload)||std::getline(request,extra))
        return Fail("read malformed slot-writer request");
    if(id.empty()||id.find_first_not_of("0123456789")!=std::string::npos)
        return Fail("slot-writer request has invalid id");
    bool ok=false;
    std::string output;
    if(operation=="stage") {
        const std::string prefix=std::string(kOtaStageRoot)+"/stage.";
        if(payload.rfind(prefix,0)!=0||payload.find("/../")!=std::string::npos||payload.size()<=prefix.size()) {
            errno=EINVAL;output="invalid stage directory";
        } else {
            std::error_code ec;
            if(!fs::is_directory(payload,ec)||ec){errno=ec?ec.value():ENOENT;output="stage directory unavailable";}
            else {ok=Stage(payload);output=ok?"stage completed":"stage failed; see slot-writer log";}
        }
    } else if(operation=="finalize"&&payload.empty()) {
        ok=Finalize();output=ok?"finalize completed":"finalize failed; see slot-writer log";
    } else if(operation=="remove-inactive"&&payload.empty()) {
        ok=RemoveInactive();output=ok?"remove-inactive completed":"remove-inactive failed; see slot-writer log";
    } else {
        errno=EINVAL;output="unsupported operation or unexpected payload directory";
    }
    return WriteResult(result_path,id,ok,output);
}
}

int main(int argc,char** argv) {
    if(argc==4&&std::strcmp(argv[1],"--request")==0)return RunRequest(argv[2],argv[3])?0:1;
    if(argc==3&&std::strcmp(argv[1],"stage")==0)return Stage(argv[2])?0:1;
    if(argc==2&&std::strcmp(argv[1],"finalize")==0)return Finalize()?0:1;
    if(argc==2&&std::strcmp(argv[1],"remove-inactive")==0)return RemoveInactive()?0:1;
    std::fprintf(stderr,"usage: matonos-slot-writer --request <request-file> <result-file>\n");return 2;
}
