#include <libfiemap/image_manager.h>

#include "service/FatFilesystem.h"

#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mi = matonos::install;
namespace {
constexpr char kMetadata[] = "/metadata/maton/images";
constexpr char kImageDir[] = "/maton_system/images";
constexpr char kEsp[] = "/dev/block/by-name/esp";
constexpr char kEntry[] = "loader/entries/B+3-0.conf";
constexpr char kUki[] = "EFI/Linux/matonos-b.efi";
constexpr char kImages[][16] = {"system", "system_ext", "product", "vendor", "odm"};
constexpr size_t kCopyBytes = 1024 * 1024;

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

bool CopyToImage(int source, const std::string& name, uint64_t size,
                 android::fiemap::ImageManager* manager) {
    if (size % 512 != 0) return Fail("payload size is not 512-byte aligned for " + name);
    if (manager->IsImageMapped(name) && !manager->UnmapImageDevice(name))
        return Fail("unmap existing inactive image " + name);
    if (manager->BackingImageExists(name) && !manager->DeleteBackingImage(name))
        return Fail("remove previous inactive image " + name);
    if (!manager->CreateBackingImage(name, size, android::fiemap::IImageManager::CREATE_IMAGE_READONLY, nullptr))
        return Fail("create libfiemap image and extent metadata for " + name);
    std::string block;
    if (!manager->MapImageDevice(name, std::chrono::seconds(30), &block))
        return Fail("map new image " + name);
    int output = open(block.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (output < 0) { manager->UnmapImageDevice(name); return Fail("open mapped image " + block); }
    std::vector<uint8_t> bytes(kCopyBytes);
    uint64_t offset=0;
    while(offset<size) {
        const size_t want=static_cast<size_t>(std::min<uint64_t>(bytes.size(),size-offset));
        ssize_t got;
        do { got=pread(source,bytes.data(),want,static_cast<off_t>(offset)); } while(got<0&&errno==EINTR);
        if(got<=0){const int e=got<0?errno:EIO;CLOSE_FD(output);manager->UnmapImageDevice(name);errno=e;return Fail("read payload for " + name);}
        size_t done=0;
        while(done<static_cast<size_t>(got)) {
            ssize_t put;
            do { put=pwrite(output,bytes.data()+done,static_cast<size_t>(got)-done,static_cast<off_t>(offset+done)); } while(put<0&&errno==EINTR);
            if(put<=0){const int e=put<0?errno:EIO;CLOSE_FD(output);manager->UnmapImageDevice(name);errno=e;return Fail("write mapped image " + name);}
            done+=static_cast<size_t>(put);
        }
        offset+=static_cast<uint64_t>(got);
    }
    if(fsync(output)!=0){const int e=errno;CLOSE_FD(output);manager->UnmapImageDevice(name);errno=e;return Fail("fsync mapped image " + name);}
    CLOSE_FD(output);
    if(!manager->UnmapImageDevice(name)) return Fail("unmap completed image " + name);
    return true;
}

bool OpenEsp(bool writable, mi::FatVolume* volume, std::string* error) {
    std::error_code ec;
    const auto device=fs::canonical(kEsp,ec);
    if(ec){*error="Cannot resolve ESP block device: "+ec.message();return false;}
    return mi::FatVolume::Open(device.string(), writable, volume, error);
}

bool ReplaceSlotOption(std::string* options) {
    size_t p=options->find("androidboot.slot_suffix=");
    if(p==std::string::npos)return false;
    const size_t end=options->find_first_of(" \r\n",p);
    options->replace(p,end==std::string::npos?options->size()-p:end-p,"androidboot.slot_suffix=_b");
    return options->find("androidboot.slot_suffix=_b")!=std::string::npos;
}

bool Stage(const fs::path& payload) {
    char current_suffix[PROP_VALUE_MAX]{};
    if(__system_property_get("ro.boot.slot_suffix",current_suffix)<=0||std::strcmp(current_suffix,"_a")!=0){
        std::fprintf(stderr,"matonos-slot-writer: this command only stages slot B while slot A is running\n");return false;
    }
    if (access("/maton_system/images", R_OK|W_OK|X_OK)!=0 || access(kMetadata,R_OK|W_OK|X_OK)!=0)
        return Fail("installed image store or libfiemap metadata is unavailable");
    mi::FatVolume esp_read;std::string preflight_error;
    if(!OpenEsp(false,&esp_read,&preflight_error)){std::fprintf(stderr,"matonos-slot-writer: preflight ESP: %s\n",preflight_error.c_str());return false;}
    std::vector<uint8_t> a_bytes;
    if(!esp_read.ReadFile("loader/entries/A.conf",&a_bytes,&preflight_error)){std::fprintf(stderr,"matonos-slot-writer: preflight A.conf: %s\n",preflight_error.c_str());return false;}
    std::string entry(a_bytes.begin(),a_bytes.end());
    bool title=false,efi=false,options=false;
    size_t begin=0;
    while(begin<entry.size()) {
        size_t end=entry.find('\n',begin);std::string line=entry.substr(begin,(end==std::string::npos?entry.size():end)-begin);
        if(line.rfind("title ",0)==0)title=true;
        else if(line.rfind("efi ",0)==0)efi=true;
        else if(line.rfind("options ",0)==0){std::string copy=line;options=ReplaceSlotOption(&copy);}
        if(end==std::string::npos) break;
        begin=end+1;
    }
    if(!title||!efi||!options){std::fprintf(stderr,"matonos-slot-writer: A.conf lacks required title, efi, or slot_suffix options\n");return false;}
    auto manager=android::fiemap::ImageManager::Open(kMetadata,kImageDir);
    if(!manager)return Fail("open libfiemap image manager");
    for(const char* base:kImages) {
        const fs::path path=payload/(std::string(base)+"_b.erofs");
        int fd=-1;uint64_t size=0;
        if(!ReadRegular(path,&fd,&size))return false;
        const bool copied=CopyToImage(fd,std::string(base)+"_b",size,manager.get());
        CLOSE_FD(fd);
        if(!copied)return false;
    }
    int vbmeta=-1;uint64_t vbmeta_size=0;
    if(!ReadRegular(payload/"vbmeta_b.img",&vbmeta,&vbmeta_size))return false;
    const bool vbmeta_ok=CopyToImage(vbmeta,"vbmeta_b",vbmeta_size,manager.get());
    CLOSE_FD(vbmeta);
    if(!vbmeta_ok)return false;

    int uki=-1;uint64_t uki_size=0;
    if(!ReadRegular(payload/"EFI/Linux/matonos-b.efi",&uki,&uki_size))return false;
    std::vector<uint8_t> uki_bytes(static_cast<size_t>(uki_size));
    size_t read=0;
    while(read<uki_bytes.size()) { ssize_t n=pread(uki,uki_bytes.data()+read,uki_bytes.size()-read,read); if(n<0&&errno==EINTR)continue; if(n<=0){CLOSE_FD(uki);return Fail("read B UKI");} read+=static_cast<size_t>(n); }
    CLOSE_FD(uki);
    mi::FatVolume esp;std::string error;
    if(!OpenEsp(true,&esp,&error)){std::fprintf(stderr,"matonos-slot-writer: open ESP: %s\n",error.c_str());return false;}
    if(!esp.WriteFile(kUki,uki_bytes,&error)||!esp.Sync(&error)){std::fprintf(stderr,"matonos-slot-writer: write/sync B UKI: %s\n",error.c_str());return false;}
    if(!esp.ReadFile("loader/entries/A.conf",&a_bytes,&error)){std::fprintf(stderr,"matonos-slot-writer: read A.conf: %s\n",error.c_str());return false;}
    entry.assign(a_bytes.begin(),a_bytes.end());
    std::string updated;begin=0;
    while(begin<entry.size()) {
        size_t end=entry.find('\n',begin);std::string line=entry.substr(begin,(end==std::string::npos?entry.size():end)-begin);
        if(line.rfind("title ",0)==0){line="title MatonOS (slot B)";title=true;}
        else if(line.rfind("efi ",0)==0){line="efi /EFI/Linux/matonos-b.efi";efi=true;}
        else if(line.rfind("options ",0)==0){if(!ReplaceSlotOption(&line)){std::fprintf(stderr,"matonos-slot-writer: A.conf has no slot_suffix option\n");return false;}options=true;}
        updated+=line;if(end!=std::string::npos)updated.push_back('\n');if(end==std::string::npos)break;begin=end+1;
    }
    if(!title||!efi||!options){std::fprintf(stderr,"matonos-slot-writer: A.conf lacks required title, efi, or options line\n");return false;}
    if(!esp.WriteFile(kEntry,std::vector<uint8_t>(updated.begin(),updated.end()),&error)||!esp.Sync(&error)){
        std::fprintf(stderr,"matonos-slot-writer: write/sync B counted entry: %s\n",error.c_str());return false;
    }
    std::fprintf(stderr,"matonos-slot-writer: staged B images, vbmeta, UKI, and counted entry; A remains default\n");
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
    auto manager=android::fiemap::ImageManager::Open(kMetadata,kImageDir);
    if(!manager){return Fail("open libfiemap image manager before finalize");}
    for(const char* base:kImages){
        const std::string name=std::string(base)+"_b";
        if(!manager->BackingImageExists(name)){std::fprintf(stderr,"matonos-slot-writer: cannot finalize; missing B image %s\n",name.c_str());return false;}
    }
    if(!manager->BackingImageExists("vbmeta_b")){std::fprintf(stderr,"matonos-slot-writer: cannot finalize; missing vbmeta_b\n");return false;}
    mi::FatVolume esp;std::string error;std::vector<uint8_t> contents;
    if(!OpenEsp(false,&esp,&error)||!esp.ReadFile(kUki,&contents,&error)||contents.empty()){
        std::fprintf(stderr,"matonos-slot-writer: cannot finalize; B UKI unavailable: %s\n",error.c_str());return false;
    }
    if(!esp.ReadFile(kEntry,&contents,&error)||contents.empty()){
        std::fprintf(stderr,"matonos-slot-writer: cannot finalize; counted B entry unavailable: %s\n",error.c_str());return false;
    }
    if(!RunBootctl("set-active-boot-slot","1"))return false;
    std::fprintf(stderr,"matonos-slot-writer: BootControl selected slot B\n");
    return true;
}

bool RemoveA() {
    char suffix[PROP_VALUE_MAX]{};
    if(__system_property_get("ro.boot.slot_suffix",suffix)<=0||std::strcmp(suffix,"_b")!=0){std::fprintf(stderr,"matonos-slot-writer: refusing to remove A unless running slot is B\n");return false;}
    if(!RunBootctl("is-slot-marked-successful","1")){std::fprintf(stderr,"matonos-slot-writer: refusing to remove A before B is marked successful\n");return false;}
    auto manager=android::fiemap::ImageManager::Open(kMetadata,kImageDir);
    if(!manager)return Fail("open libfiemap image manager for explicit A removal");
    for(const char* base:kImages){const std::string name=std::string(base)+"_a";if(manager->IsImageMapped(name)&&!manager->UnmapImageDevice(name))return Fail("unmap old A image "+name);if(manager->BackingImageExists(name)&&!manager->DeleteBackingImage(name))return Fail("delete old A image "+name);}
    if(manager->IsImageMapped("vbmeta_a")&&!manager->UnmapImageDevice("vbmeta_a"))return Fail("unmap old vbmeta_a");
    if(manager->BackingImageExists("vbmeta_a")&&!manager->DeleteBackingImage("vbmeta_a"))return Fail("delete old vbmeta_a");
    std::fprintf(stderr,"matonos-slot-writer: explicitly removed successful old slot A\n");return true;
}
}

int main(int argc,char** argv) {
    if(argc==3&&std::strcmp(argv[1],"stage")==0)return Stage(argv[2])?0:1;
    if(argc==2&&std::strcmp(argv[1],"finalize")==0)return Finalize()?0:1;
    if(argc==2&&std::strcmp(argv[1],"remove-old-a")==0)return RemoveA()?0:1;
    std::fprintf(stderr,"usage: matonos-slot-writer stage <payload-dir> | finalize | remove-old-a\n");return 2;
}
