#include "InstallerService.h"
#include "EfiBootEntry.h"
#include "DriveHealth.h"
#include "Json.h"
#include "matonos_ipc.h"

#include <android/log.h>
#include "Props.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/dm-ioctl.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <algorithm>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string_view>
#include <sstream>
#include <unistd.h>
#include <vector>
#include <iterator>
#include <fstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <thread>

#define CLOSE_FD(fd_value) do { \
    const int close_fd_value = (fd_value); \
    if (close_fd_value >= 0) { \
        std::fprintf(stderr, "installer: close fd=%d at %s:%d\n", close_fd_value, __FILE__, __LINE__); \
        if (::close(close_fd_value) != 0) std::fprintf(stderr, "installer: close fd=%d failed errno=%d at %s:%d\n", close_fd_value, errno, __FILE__, __LINE__); \
    } \
} while (0)

namespace mi = matonos::install;
namespace js = matonos::install::json;
namespace {
constexpr char kTag[] = "MatonosInstaller";
constexpr char kImageStoreRoot[] = "/mnt/matonos-install";
constexpr char kSystemMapper[] = "matonos-install-system";
constexpr char kOtaStageRoot[] = "/maton_system/ota-staging";
constexpr char kSlotWriterRequest[] = "/maton_system/ota-staging/.request";
constexpr char kSlotWriterResult[] = "/maton_system/ota-staging/.result";
constexpr const char* kOtaNames[] = {"manifest.json", "manifest.sig", "system.erofs",
    "system_ext.erofs", "product.erofs", "vendor.erofs", "odm.erofs", "vbmeta.img",
    "matonos-a.efi", "matonos-b.efi"};
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_operation_active{false};
std::mutex g_operation_mutex;
MatonosIpcServer* g_server = nullptr;
bool LiveMode();
int HelperFail(const char* operation,const char* target,int error_number);

bool VerifyPartitionFd(const char* path, uint64_t expected, int* out, std::string* error) {
    const int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){*error="open target partition "+std::string(path)+": "+strerror(errno);return false;}
    struct stat st{};uint64_t seq=0;
    if(!expected||fstat(fd,&st)!=0||!S_ISBLK(st.st_mode)||ioctl(fd,BLKGETDISKSEQ,&seq)!=0||seq!=expected){
        const int e=errno;CLOSE_FD(fd);*error="target partition identity changed for "+std::string(path)+": "+strerror(e);return false;
    }
    *out=fd;return true;
}

bool RunMke2fs(const std::string& device,const char* label,std::string* error) {
    const pid_t pid=fork();
    if(pid<0){*error="start mke2fs: "+std::string(strerror(errno));return false;}
    if(pid==0){
        constexpr char fd_prefix[]="/proc/self/fd/";
        if(device.rfind(fd_prefix,0)==0){
            char* end=nullptr;const long fd=strtol(device.c_str()+sizeof(fd_prefix)-1,&end,10);
            if(!end||*end||fd<0||fd>INT_MAX||fcntl(static_cast<int>(fd),F_SETFD,0)<0)_exit(126);
        }
        execl("/system/bin/mke2fs","mke2fs","-t","ext4","-F","-L",label,device.c_str(),static_cast<char*>(nullptr));_exit(127);
    }
    int status=0;while(waitpid(pid,&status,0)<0){if(errno==EINTR)continue;*error="wait for mke2fs: "+std::string(strerror(errno));return false;}
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){*error="mke2fs failed for "+device;return false;}return true;
}

bool RemoveDmDevice(const char* name) {
    const int control=open("/dev/device-mapper",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(control<0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: open /dev/device-mapper for %s removal failed errno=%d",name,errno);return false;}
    dm_ioctl io{};io.version[0]=DM_VERSION_MAJOR;io.version[1]=DM_VERSION_MINOR;io.version[2]=DM_VERSION_PATCHLEVEL;io.data_size=sizeof(io);io.data_start=sizeof(io);strlcpy(io.name,name,sizeof(io.name));
    if(ioctl(control,DM_DEV_REMOVE,&io)!=0){const int e=errno;CLOSE_FD(control);__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: DM_DEV_REMOVE %s failed errno=%d",name,e);return false;}
    CLOSE_FD(control);return true;
}

bool UnmountImagePath(const std::string& path) {
    (void)path;
    const std::string id=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if(!matonos::props::Set("vendor.maton.install.unmount.request",id) ||
       !matonos::props::WaitFor("vendor.maton.install.unmount.done",id,std::chrono::seconds(10))){
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: init unmount request %s timed out",id.c_str());return false;
    }
    std::ifstream mounts("/proc/self/mountinfo");std::string line;
    while(std::getline(mounts,line))if(line.find(" /mnt/matonos-install/")!=std::string::npos){
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: target image-store mount remains after init unmount: %s",line.c_str());return false;
    }
    return true;
}

bool MountImageStore(const std::string& metadata,const std::string& system,std::string* error){
    const std::string id=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if(!matonos::props::Set("vendor.maton.install.metadata_source",metadata)||
       !matonos::props::Set("vendor.maton.install.system_source",system)||
       !matonos::props::Set("vendor.maton.install.mount.request",id)||
       !matonos::props::WaitFor("vendor.maton.install.mount.done",id,std::chrono::seconds(10))){
        *error="init image-store mount request "+id+" timed out";return false;
    }
    std::ifstream mounts("/proc/self/mountinfo");std::string line;bool metadata_mounted=false,system_mounted=false;
    while(std::getline(mounts,line)){
        if(line.find(" /mnt/matonos-install/metadata ")!=std::string::npos)metadata_mounted=true;
        if(line.find(" /mnt/matonos-install/system ")!=std::string::npos)system_mounted=true;
    }
    if(!metadata_mounted||!system_mounted){*error="init reported image-store mount complete but mountinfo is incomplete";UnmountImagePath("/mnt/matonos-install");return false;}
    return true;
}

bool CreateLinearDm(int fd,const char* name,std::string* path,std::string* error) {
    uint64_t bytes=0;struct stat st{};
    if(ioctl(fd,BLKGETSIZE64,&bytes)!=0||fstat(fd,&st)!=0||bytes<4096||bytes%512){*error="invalid maton_system block device size";return false;}
    const int control=open("/dev/device-mapper",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(control<0){*error="open /dev/device-mapper: "+std::string(strerror(errno));return false;}
    std::vector<uint8_t> buffer(4096);auto* io=reinterpret_cast<dm_ioctl*>(buffer.data());io->version[0]=DM_VERSION_MAJOR;io->version[1]=DM_VERSION_MINOR;io->version[2]=DM_VERSION_PATCHLEVEL;io->data_size=buffer.size();io->data_start=sizeof(dm_ioctl);strlcpy(io->name,name,sizeof(io->name));
    if(ioctl(control,DM_DEV_CREATE,io)!=0){const int e=errno;CLOSE_FD(control);*error="DM_DEV_CREATE for install image store: "+std::string(strerror(e));return false;}
    const uint32_t dm_dev=io->dev;std::fill(buffer.begin(),buffer.end(),0);io=reinterpret_cast<dm_ioctl*>(buffer.data());io->version[0]=DM_VERSION_MAJOR;io->version[1]=DM_VERSION_MINOR;io->version[2]=DM_VERSION_PATCHLEVEL;io->data_size=buffer.size();io->data_start=sizeof(dm_ioctl);io->target_count=1;strlcpy(io->name,name,sizeof(io->name));
    auto* target=reinterpret_cast<dm_target_spec*>(buffer.data()+sizeof(dm_ioctl));target->sector_start=0;target->length=bytes/512;strlcpy(target->target_type,"linear",sizeof(target->target_type));
    const int used=snprintf(reinterpret_cast<char*>(target+1),buffer.size()-sizeof(dm_ioctl)-sizeof(dm_target_spec),"%u:%u 0",major(st.st_rdev),minor(st.st_rdev));
    if(used<0||static_cast<size_t>(used)>=buffer.size()-sizeof(dm_ioctl)-sizeof(dm_target_spec)){CLOSE_FD(control);RemoveDmDevice(name);*error="linear device-mapper table is too large";return false;}
    target->next=static_cast<uint32_t>(sizeof(dm_target_spec)+used+1);target->next=(target->next+7u)&~7u;
    if(ioctl(control,DM_TABLE_LOAD,io)!=0){const int e=errno;CLOSE_FD(control);RemoveDmDevice(name);*error="DM_TABLE_LOAD for install image store: "+std::string(strerror(e));return false;}
    std::fill(buffer.begin(),buffer.end(),0);io=reinterpret_cast<dm_ioctl*>(buffer.data());io->version[0]=DM_VERSION_MAJOR;io->version[1]=DM_VERSION_MINOR;io->version[2]=DM_VERSION_PATCHLEVEL;io->data_size=buffer.size();io->data_start=sizeof(dm_ioctl);io->flags=0;strlcpy(io->name,name,sizeof(io->name));
    if(ioctl(control,DM_DEV_SUSPEND,io)!=0){const int e=errno;CLOSE_FD(control);RemoveDmDevice(name);*error="DM_DEV_SUSPEND resume for install image store: "+std::string(strerror(e));return false;}
    CLOSE_FD(control);
    *path=std::string("/dev/block/dm-")+std::to_string(minor(dm_dev));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(access(path->c_str(),R_OK|W_OK)!=0){if(std::chrono::steady_clock::now()>deadline){const int e=errno;RemoveDmDevice(name);*error="device-mapper node did not appear: "+*path+": "+strerror(e);return false;}std::this_thread::sleep_for(std::chrono::milliseconds(50));}
    return true;
}

int PrepareImageStore(uint64_t expected,const char* system_path,const char* metadata_path) {
    std::string error;int system_fd=-1,metadata_fd=-1;
    if(!VerifyPartitionFd(system_path,expected,&system_fd,&error))return HelperFail("prepare image store",error.c_str(),errno);
    if(!VerifyPartitionFd(metadata_path,expected,&metadata_fd,&error)){CLOSE_FD(system_fd);return HelperFail("prepare image store",error.c_str(),errno);}
    std::string mapped;
    if(!CreateLinearDm(system_fd,kSystemMapper,&mapped,&error)){CLOSE_FD(system_fd);CLOSE_FD(metadata_fd);return HelperFail("create linear image-store device",error.c_str(),errno);}
    if(!RunMke2fs("/proc/self/fd/"+std::to_string(metadata_fd),"metadata",&error)||!RunMke2fs(mapped,"maton_system",&error)){RemoveDmDevice(kSystemMapper);CLOSE_FD(system_fd);CLOSE_FD(metadata_fd);return HelperFail("format image store",error.c_str(),errno);}
    std::error_code dir_error;
    std::filesystem::create_directories(std::string(kImageStoreRoot)+"/metadata",dir_error);
    if(dir_error){RemoveDmDevice(kSystemMapper);CLOSE_FD(system_fd);CLOSE_FD(metadata_fd);return HelperFail("create metadata mount directory",dir_error.message().c_str(),dir_error.value());}
    std::filesystem::create_directories(std::string(kImageStoreRoot)+"/system",dir_error);
    if(dir_error){RemoveDmDevice(kSystemMapper);CLOSE_FD(system_fd);CLOSE_FD(metadata_fd);return HelperFail("create system mount directory",dir_error.message().c_str(),dir_error.value());}
    // Keep the verified metadata fd open while init mounts it. Passing the
    // original /dev/block path would reopen a possibly replaced device after
    // hot-unplug; the proc fd path pins the checked block device.
    const std::string metadata_fd_path="/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(metadata_fd);
    if(!MountImageStore(metadata_fd_path,mapped,&error)){CLOSE_FD(system_fd);CLOSE_FD(metadata_fd);RemoveDmDevice(kSystemMapper);return HelperFail("init mount target image store",error.c_str(),errno);}
    CLOSE_FD(system_fd);CLOSE_FD(metadata_fd);
    std::filesystem::create_directories(std::string(kImageStoreRoot)+"/metadata/maton/images",dir_error);
    if(!dir_error)std::filesystem::create_directories(std::string(kImageStoreRoot)+"/system/images",dir_error);
    if(dir_error){const int e=dir_error.value();UnmountImagePath(std::string(kImageStoreRoot)+"/system");RemoveDmDevice(kSystemMapper);return HelperFail("create image directories",dir_error.message().c_str(),e);}
    return 0;
}

int FinishImageStore() {
    bool ok=UnmountImagePath(kImageStoreRoot);
    if(!RemoveDmDevice(kSystemMapper))ok=false;
    return ok?0:1;
}

std::string Escape(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: if (c >= 0x20 && c < 0x80) out.push_back(static_cast<char>(c));
                else if (c >= 0x80) { static constexpr char hex[] = "0123456789abcdef"; out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
        }
    }
    return out + "\"";
}
const js::Value* Get(const js::Value& v, const char* key) { return v.Get(key); }
std::string S(const js::Value& v, const char* key) { const auto* p = Get(v, key); return p ? p->String() : ""; }
uint64_t N(const js::Value& v, const char* key) { const auto* p = Get(v, key); return p ? p->Number() : 0; }
mi::PartitionRef ParseRef(const js::Value& value) {
    return {S(value, "partGuid"), S(value, "logicalName")};
}
bool ParseOperation(const js::Value& value, mi::Operation* out, std::string* error) {
    const std::string kind = S(value, "kind");
    if (kind == "write_gpt") {
        mi::WriteGpt op; op.declared_disk_bytes = N(value, "declaredDiskBytes"); op.disk_guid = S(value, "diskGuid");
        const auto* parts = Get(value, "partitions");
        if (!parts || parts->kind != js::Value::Kind::kArray) { *error = "GPT partition list is missing."; return false; }
        for (const auto& item : parts->array) op.partitions.push_back({S(item,"name"), S(item,"typeGuid"), S(item,"partGuid"), N(item,"startBytes"), N(item,"sizeBytes")});
        *out = std::move(op); return true;
    }
    if (kind == "populate_images") {
        mi::PopulateImages op{S(value,"matonSystemPartGuid"),S(value,"metadataPartGuid"),{}};
        const auto* images=Get(value,"images");
        if (!images || images->kind != js::Value::Kind::kArray) { *error="Image list is missing."; return false; }
        for (const auto& image : images->array)
            op.images.push_back({S(image,"name"),S(image,"livePartitionName")});
        *out=std::move(op); return true;
    }
    if (kind == "create_lp_metadata") {
        mi::CreateLpMetadata op; op.super_part_guid = S(value,"superPartGuid"); op.metadata_size_bytes = N(value,"metadataSizeBytes"); const uint64_t slots = N(value,"metadataSlots");
        if (slots > UINT32_MAX) { *error = "LP metadata slot count is out of range."; return false; }
        op.metadata_slots = static_cast<uint32_t>(slots);
        const auto* groups = Get(value,"groups");
        if (!groups || groups->kind != js::Value::Kind::kArray) { *error = "LP groups are missing."; return false; }
        for (const auto& group : groups->array) {
            mi::LogicalGroupSpec item{S(group,"name"),N(group,"maximumSizeBytes"),{}};
            const auto* members=Get(group,"partitions");
            if (!members || members->kind != js::Value::Kind::kArray) { *error = "LP members are missing."; return false; }
            for (const auto& member : members->array) item.partitions.push_back({S(member,"name"),N(member,"sizeBytes")});
            op.groups.push_back(std::move(item));
        }
        *out = std::move(op); return true;
    }
    if (kind == "format") {
        mi::Filesystem fs;
        const auto name = S(value,"filesystem");
        if (name == "vfat") fs=mi::Filesystem::kVfat;
        else if (name == "ext4") fs=mi::Filesystem::kExt4;
        else if (name == "f2fs") fs=mi::Filesystem::kF2fs;
        else { *error = "Unsupported filesystem."; return false; }
        const auto* target=Get(value,"target"); if (!target) { *error="Format target missing."; return false; }
        *out=mi::Format{ParseRef(*target),fs,S(value,"label")}; return true;
    }
    if (kind == "copy_partition") {
        const auto* source=Get(value,"source"), *target=Get(value,"target");
        if (!source || !target) { *error="Copy source or target missing."; return false; }
        const std::string source_kind=S(*source,"kind");
        if (source_kind == "live_image") *out=mi::CopyPartition{mi::CopySourceKind::kLiveImagePartition,S(*source,"partitionName"),{},ParseRef(*target)};
        else if (source_kind == "target") { const auto* ref=Get(*source,"partition"); if (!ref) { *error="Target copy source missing."; return false; } *out=mi::CopyPartition{mi::CopySourceKind::kTargetPartition,"",ParseRef(*ref),ParseRef(*target)}; }
        else { *error="Unsupported copy source."; return false; }
        return true;
    }
    if (kind == "clone_partition") {
        const auto* source=Get(value,"source"), *target=Get(value,"target"); if (!source||!target) { *error="Clone source or target missing."; return false; }
        *out=mi::ClonePartition{ParseRef(*source),ParseRef(*target)}; return true;
    }
    if (kind == "register_boot_entry") {
        const auto* esp=Get(value,"esp"); if (!esp) { *error="Boot entry ESP missing."; return false; }
        *out=mi::RegisterBootEntry{ParseRef(*esp)}; return true;
    }
    if (kind == "write_files") {
        const auto* target=Get(value,"target"), *files=Get(value,"files"); if (!target||!files||files->kind!=js::Value::Kind::kArray) { *error="File payload is missing."; return false; }
        mi::WriteFiles op{ParseRef(*target),S(value,"relativeDirectory"),{}};
        for (const auto& file:files->array) op.files.push_back({S(file,"relativePath"),S(file,"livePayloadPath"),S(file,"inlineContents"),S(file,"sha256")});
        *out=std::move(op); return true;
    }
    *error = "Unsupported installer operation kind.";
    return false;
}
bool LiveMode() {
    std::ifstream f("/proc/cmdline"); if (!f) return false;
    std::string line((std::istreambuf_iterator<char>(f)), {});
    return line.find("androidboot.matonos.live=1") != std::string::npos;
}
std::vector<mi::Drive> Snapshot() {
    return mi::EnumerateDrives("/sys/block","/dev/block","/proc/self/mountinfo","/proc/cmdline","/proc/swaps");
}
// SMART needs the helper's capabilities; the daemon only relays its JSON.
std::string HealthJson(const mi::Drive& d) {
    static const std::string unknown="{\"known\":false,\"source\":\"\",\"findings\":[]}";
    int pipe_fds[2];
    if(pipe2(pipe_fds,O_CLOEXEC)!=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: health pipe: %s",strerror(errno));return unknown;}
    const pid_t pid=fork();
    if(pid<0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: health fork: %s",strerror(errno));CLOSE_FD(pipe_fds[0]);CLOSE_FD(pipe_fds[1]);return unknown;}
    if(pid==0) {
        if(dup2(pipe_fds[1],STDOUT_FILENO)<0)_exit(126);
        const std::string seq=std::to_string(d.disk_sequence);
        execl("/odm/bin/matonos-installer-service","/odm/bin/matonos-installer-service","--health",seq.c_str(),d.path.c_str(),nullptr);
        _exit(127);
    }
    CLOSE_FD(pipe_fds[1]);
    std::string json;char buffer[1024];
    for(;;){const ssize_t n=read(pipe_fds[0],buffer,sizeof(buffer));if(n<0&&errno==EINTR)continue;if(n<=0)break;json.append(buffer,static_cast<size_t>(n));}
    CLOSE_FD(pipe_fds[0]);
    int status=0;
    while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0||json.empty()||json.front()!='{') {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: health helper for %s failed (status %d)",d.path.c_str(),status);
        return unknown;
    }
    return json;
}
std::string DrivesJson() {
    std::ostringstream out; out << "{\"drives\":[";
    const auto drives=Snapshot();
    for (size_t i=0;i<drives.size();++i) {
        const auto& d=drives[i]; if (i) out << ',';
        out << "{\"id\":"<<Escape(d.id)<<",\"identity\":"<<Escape(d.identity)<<",\"path\":"<<Escape(d.path)<<",\"model\":"<<Escape(d.model)<<",\"serial\":"<<Escape(d.serial)
            <<",\"sizeBytes\":"<<d.size_bytes<<",\"transport\":"<<Escape(d.transport)<<",\"removable\":"<<(d.removable?"true":"false")<<",\"rotational\":"<<(d.rotational?"true":"false")
            <<",\"safe\":"<<(d.safe?"true":"false")<<",\"reason\":"<<Escape(d.unsafe_reason)<<",\"health\":"<<HealthJson(d)<<",\"partitions\":[";
        for (size_t j=0;j<d.partitions.size();++j) { const auto& p=d.partitions[j]; if(j)out<<',';
            out<<"{\"name\":"<<Escape(p.name)<<",\"partGuid\":"<<Escape(p.part_guid)<<",\"typeGuid\":"<<Escape(p.type_guid)
               <<",\"startBytes\":"<<p.start_bytes<<",\"sizeBytes\":"<<p.size_bytes<<",\"mounted\":"<<(p.mounted?"true":"false")<<",\"logical\":"<<(p.logical?"true":"false")<<'}'; }
        out << "]}";
    }
    return out.str()+"]}";
}
int GetStatus(const char*, char* result, size_t capacity, void*) {
    // Destructive operations still require a fresh disk identity and explicit UI confirmation.
    const std::string json="{\"executorReady\":true,\"dryRunVerified\":true,\"readbackVerified\":true,\"liveMode\":"+std::string(LiveMode()?"true":"false")+"}";
    if (json.size()+1>capacity) return -1; std::memcpy(result,json.c_str(),json.size()+1); return 0;
}

int RunSlotWriter(const char* command, const char* payload_dir) {
    if(std::strcmp(command,"stage")!=0&&std::strcmp(command,"finalize")!=0&&
       std::strcmp(command,"remove-inactive")!=0){
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: unsupported slot-writer step=%s",command);return -1;
    }
    const std::string id=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string request=id+"\n"+command+"\n"+(payload_dir?payload_dir:"")+"\n";
    const int fd=open(kSlotWriterRequest,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: open slot-writer request step=%s path=%s failed errno=%d",command,kSlotWriterRequest,errno);return -1;}
    size_t written=0;
    while(written<request.size()){
        const ssize_t n=write(fd,request.data()+written,request.size()-written);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){const int e=n<0?errno:EIO;CLOSE_FD(fd);__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: write slot-writer request step=%s failed errno=%d",command,e);return -1;}
        written+=static_cast<size_t>(n);
    }
    if(fsync(fd)!=0){const int e=errno;CLOSE_FD(fd);__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: sync slot-writer request step=%s failed errno=%d",command,e);return -1;}
    CLOSE_FD(fd);
    if(!matonos::props::Set("vendor.maton.install.slot_writer.request",id)){
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: set slot-writer request property step=%s request=%s failed",command,id.c_str());return -1;
    }
    if(!matonos::props::WaitFor("vendor.maton.install.slot_writer.done",id,std::chrono::minutes(30))){
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: timed out waiting for init slot-writer step=%s request=%s",command,id.c_str());return -1;
    }
    std::ifstream response(kSlotWriterResult);
    std::string result_id,status_text,output;
    if(!std::getline(response,result_id)||!std::getline(response,status_text)||result_id!=id){
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: missing or stale slot-writer result step=%s request=%s",command,id.c_str());return -1;
    }
    std::ostringstream rest;rest<<response.rdbuf();output=rest.str();
    if(status_text!="0"){
        __android_log_print(ANDROID_LOG_ERROR,kTag,"installer: slot-writer step=%s failed request=%s output=%s",command,id.c_str(),output.c_str());return -1;
    }
    __android_log_print(ANDROID_LOG_INFO,kTag,"installer: slot-writer step=%s completed request=%s",command,id.c_str());
    return 0;
}

int StageUpdate(const char* const* names,const int* fds,size_t count,char* result,size_t capacity,void*) {
    if(count!=std::size(kOtaNames)){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: OTA stage expects %zu descriptors, received %zu",std::size(kOtaNames),count);return -1;}
    uint64_t total=0;
    for(size_t i=0;i<count;++i){
        if(!names[i]||std::strcmp(names[i],kOtaNames[i])!=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: OTA stage descriptor %zu has unexpected name %s",i,names[i]?names[i]:"<null>");return -1;}
        struct stat st{};if(fstat(fds[i],&st)!=0||!S_ISREG(st.st_mode)||st.st_size<=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: OTA stage input %s is not a readable regular file fd=%d errno=%d",names[i],fds[i],errno);return -1;}
        if(static_cast<uint64_t>(st.st_size)>UINT64_MAX-total){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: OTA payload size overflow at %s",names[i]);return -1;}
        total+=static_cast<uint64_t>(st.st_size);
    }
    struct statvfs space{};if(statvfs(kOtaStageRoot,&space)!=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: statvfs OTA staging directory %s failed errno=%d",kOtaStageRoot,errno);return -1;}
    if(static_cast<unsigned __int128>(space.f_bavail)*space.f_frsize<total){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: insufficient space to stage OTA descriptors bytes=%llu",static_cast<unsigned long long>(total));return -1;}
    std::string pattern=std::string(kOtaStageRoot)+"/stage.XXXXXX";
    char* directory=mkdtemp(pattern.data());if(!directory){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: create OTA stage directory under %s failed errno=%d",kOtaStageRoot,errno);return -1;}
    bool ok=true;
    for(size_t i=0;i<count&&ok;++i){
        const std::string path=std::string(directory)+"/"+kOtaNames[i];
        const int output=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(output<0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: create OTA staging file %s failed errno=%d",path.c_str(),errno);ok=false;break;}
        struct stat st{};if(fstat(fds[i],&st)!=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: stat OTA descriptor %s fd=%d failed errno=%d",names[i],fds[i],errno);ok=false;}
        std::vector<uint8_t> buffer(1024*1024);uint64_t offset=0;const uint64_t size=st.st_size;
        while(ok&&offset<size){const size_t want=static_cast<size_t>(std::min<uint64_t>(buffer.size(),size-offset));ssize_t n;do{n=pread(fds[i],buffer.data(),want,static_cast<off_t>(offset));}while(n<0&&errno==EINTR);if(n<=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: read OTA descriptor %s fd=%d offset=%llu failed errno=%d",names[i],fds[i],static_cast<unsigned long long>(offset),n<0?errno:EIO);ok=false;break;}size_t done=0;while(done<static_cast<size_t>(n)){ssize_t w;do{w=write(output,buffer.data()+done,static_cast<size_t>(n)-done);}while(w<0&&errno==EINTR);if(w<=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: write OTA staging file %s offset=%llu failed errno=%d",path.c_str(),static_cast<unsigned long long>(offset+done),w<0?errno:EIO);ok=false;break;}done+=static_cast<size_t>(w);}offset+=static_cast<uint64_t>(n);}
        if(ok&&fsync(output)!=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: fsync OTA staging file %s failed errno=%d",path.c_str(),errno);ok=false;}
        CLOSE_FD(output);
    }
    if(ok)ok=RunSlotWriter("stage",directory)==0;
    std::error_code cleanup_error;std::filesystem::remove_all(directory,cleanup_error);if(cleanup_error){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: remove OTA stage directory %s failed errno=%d message=%s",directory,cleanup_error.value(),cleanup_error.message().c_str());ok=false;}
    const char* response=ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"stage_failed\"}";
    if(std::strlen(response)+1>capacity)return -1;std::memcpy(result,response,std::strlen(response)+1);return 0;
}
int FinalizeUpdate(const char*,char* result,size_t capacity,void*) {
    const bool ok=RunSlotWriter("finalize",nullptr)==0;const char* response=ok?"{\"ok\":true}":"{\"ok\":false,\"error\":\"finalize_failed\"}";
    if(std::strlen(response)+1>capacity)return -1;std::memcpy(result,response,std::strlen(response)+1);return 0;
}
int UpdateStatus(const char*,char* result,size_t capacity,void*) {
    char suffix[PROP_VALUE_MAX]{};const int len=__system_property_get("ro.boot.slot_suffix",suffix);
    if(len<=0){__android_log_print(ANDROID_LOG_ERROR,kTag,"installer: update status cannot read ro.boot.slot_suffix errno=%d",errno);return -1;}
    const std::string json=std::string("{\"available\":")+(access("/maton_system/images",R_OK|W_OK|X_OK)==0?"true":"false")+",\"runningSlot\":\""+suffix+"\"}";
    if(json.size()+1>capacity)return -1;std::memcpy(result,json.c_str(),json.size()+1);return 0;
}
int ListDrives(const char*, char* result, size_t capacity, void*) {
    const auto json=DrivesJson(); if(json.size()+1>capacity)return -1; std::memcpy(result,json.c_str(),json.size()+1); return 0;
}
void Progress(const mi::OperationProgress& p) {
    if (!g_server) return;
    std::ostringstream out; out << "{\"bytesDone\":"<<p.bytes_done<<",\"bytesTotal\":"<<p.bytes_total<<",\"message\":"<<Escape(p.message)<<'}';
    matonos_ipc_publish(g_server,"operation_progress",out.str().c_str());
}
int Cancel(const char*, char* result, size_t capacity, void*) {
    const bool active=g_operation_active.load();
    if(active)g_cancel.store(true);
    const char* response=active?"true":"false"; const size_t length=std::strlen(response)+1;
    if(capacity<length)return -1; std::memcpy(result,response,length); return 0;
}
int Execute(const char* json, char* result, size_t capacity, void*) {
    std::lock_guard<std::mutex> lock(g_operation_mutex); g_cancel.store(false);
    g_operation_active.store(true);
    struct Finish { ~Finish(){g_operation_active.store(false);} } finish;
    js::Value root; mi::OperationRequest request; mi::Operation operation; std::string error;
    const js::Value* operation_value = nullptr;
    if (!json || !js::Parse(json,&root) || root.kind!=js::Value::Kind::kObject || N(root,"apiVersion")!=1) error="Malformed or unsupported OperationRequestV1.";
    else if (!(operation_value=Get(root,"operation"))) error="OperationRequestV1 has no operation.";
    else if (!ParseOperation(*operation_value,&operation,&error)) {}
    else { request.api_version=1; request.target_disk_id=S(root,"targetDiskId"); request.target_disk_identity=S(root,"targetDiskIdentity"); request.operation=std::move(operation); }
    mi::ValidationResult validation;
    if (error.empty()) validation=mi::ExecuteOne(request,Snapshot,mi::ExecutePrimitive,LiveMode(),Progress,[]{return g_cancel.load();});
    const bool ok=error.empty()&&validation.ok;
    const std::string message=ok?"Primitive operation completed.":(error.empty()?validation.message:error);
    const std::string response=std::string("{\"ok\":")+(ok?"true":"false")+",\"message\":"+Escape(message)+"}";
    if(response.size()+1>capacity)return -1; std::memcpy(result,response.c_str(),response.size()+1); return 0;
}
}  // namespace

namespace {
int HelperFail(const char* what,const char* path,int err) {
    __android_log_print(ANDROID_LOG_ERROR,kTag,"storage helper: %s %s: %s",what,path,err?strerror(err):"check failed");
    return 1;
}

// BLKRRPART returns before ueventd has created the new partition nodes; the
// next operation opens them by path, so wait until sysfs and /dev agree.
// Opens `path` and proves it is still the block device the plan verified.
int OpenExpectedBlock(const char* path,uint64_t expected,int flags,struct stat* st) {
    const int fd=open(path,flags|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){HelperFail("open",path,errno);return -1;}
    uint64_t actual=0;
    if(!expected||fstat(fd,st)!=0||!S_ISBLK(st->st_mode)||ioctl(fd,BLKGETDISKSEQ,&actual)!=0||actual!=expected){
        CLOSE_FD(fd);HelperFail("not the verified block device:",path,0);return -1;
    }
    return fd;
}

// Whole-drive TRIM before the new layout. Drives without discard (HDDs, most
// USB bridges) only log: trimming is an optimisation, never an install gate.
int DiscardDisk(uint64_t expected,const char* disk) {
    struct stat st{};
    const int fd=OpenExpectedBlock(disk,expected,O_RDWR,&st);
    if(fd<0)return 1;
    uint64_t size=0;
    if(ioctl(fd,BLKGETSIZE64,&size)!=0||size==0){const int e=errno;CLOSE_FD(fd);return HelperFail("BLKGETSIZE64",disk,e);}
    uint64_t range[2]={0,size};
    const auto started=std::chrono::steady_clock::now();
    if(ioctl(fd,BLKDISCARD,range)!=0) {
        const int e=errno;
        __android_log_print(e==EOPNOTSUPP?ANDROID_LOG_INFO:ANDROID_LOG_ERROR,kTag,
                            "storage helper: BLKDISCARD %s (%llu bytes) not done: %s; continuing without trim",
                            disk,static_cast<unsigned long long>(size),strerror(e));
    } else {
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        __android_log_print(ANDROID_LOG_INFO,kTag,"storage helper: trimmed %s (%llu bytes) in %lld ms",
                            disk,static_cast<unsigned long long>(size),static_cast<long long>(ms));
    }
    CLOSE_FD(fd);
    return 0;
}

bool ReadSysfsNumber(const std::string& path,uint64_t* value) {
    std::ifstream in(path);unsigned long long v=0;
    if(!(in>>v))return false;
    *value=v;return true;
}

// Prints the drive's SMART findings as JSON on stdout for list_drives.
int PrintDriveHealth(uint64_t expected,const char* disk) {
    struct stat st{};
    const int fd=OpenExpectedBlock(disk,expected,O_RDONLY,&st);
    mi::DriveHealth health;
    if(fd>=0) {
        const std::string name=std::string(disk).substr(std::string(disk).rfind('/')+1);
        uint64_t rotational=1;
        ReadSysfsNumber("/sys/block/"+name+"/queue/rotational",&rotational);
        health=mi::ReadDriveHealth(fd,name.rfind("nvme",0)==0,rotational!=0);
        CLOSE_FD(fd);
    }
    const std::string json=mi::HealthToJson(health);
    std::fwrite(json.data(),1,json.size(),stdout);
    return std::fflush(stdout)==0?0:1;
}

// Registers \EFI\BOOT\BOOTX64.EFI on the new ESP in NVRAM. Geometry comes from
// sysfs for the verified partition node; failure is logged and not fatal.
int WriteEfiBootEntry(uint64_t expected,const char* esp,const char* part_guid) {
    struct stat st{};
    const int fd=OpenExpectedBlock(esp,expected,O_RDONLY,&st);
    if(fd<0)return 1;
    CLOSE_FD(fd);
    const std::string sys="/sys/dev/block/"+std::to_string(major(st.st_rdev))+":"+std::to_string(minor(st.st_rdev));
    uint64_t number=0,start=0,size=0,logical=0;
    if(!ReadSysfsNumber(sys+"/partition",&number)||!ReadSysfsNumber(sys+"/start",&start)||
       !ReadSysfsNumber(sys+"/size",&size)||!ReadSysfsNumber(sys+"/../queue/logical_block_size",&logical)||
       number==0||number>UINT32_MAX||logical<512||logical%512!=0) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"storage helper: ESP geometry unreadable under %s; no NVRAM boot entry",sys.c_str());
        return 0;
    }
    const uint64_t per=logical/512;
    mi::EfiHardDrive hd{static_cast<uint32_t>(number),start/per,size/per,part_guid};
    std::string error;
    if(access("/sys/firmware/efi/efivars",F_OK)!=0) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"storage helper: efivarfs missing (%s); no NVRAM boot entry",strerror(errno));
        return 0;
    }
    if(!mi::AddEfiBootEntry("/sys/firmware/efi/efivars",hd,&error)) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"storage helper: NVRAM boot entry not written: %s; ESP fallback loader still boots",error.c_str());
        return 0;
    }
    __android_log_print(ANDROID_LOG_INFO,kTag,"storage helper: NVRAM boot entry for %s (partition %llu, GUID %s)",
                        esp,static_cast<unsigned long long>(number),part_guid);
    return 0;
}

bool WaitForPartitionNodes(const char* disk_path) {
    const std::string disk=std::string(disk_path).substr(std::string(disk_path).rfind('/')+1);
    const std::string sys="/sys/block/"+disk;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;) {
        bool all=true;
        if(DIR* dir=opendir(sys.c_str())) {
            while(dirent* e=readdir(dir)) {
                if(std::strncmp(e->d_name,disk.c_str(),disk.size())!=0)continue;
                if(access((sys+"/"+e->d_name+"/partition").c_str(),F_OK)!=0)continue;
                if(access((std::string("/dev/block/")+e->d_name).c_str(),F_OK)!=0){all=false;break;}
            }
            closedir(dir);
        } else return HelperFail("open sysfs",sys.c_str(),errno),false;
        if(all)return true;
        if(std::chrono::steady_clock::now()>deadline) {
            __android_log_print(ANDROID_LOG_ERROR,kTag,"storage helper: partition nodes for %s did not appear within 10 s",disk_path);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
}  // namespace

int HelperMain(int argc,char** argv) {
    if(argc==5&&std::strcmp(argv[1],"--prepare-image-store")==0) {
        const uint64_t expected=std::strtoull(argv[2],nullptr,10);
        return PrepareImageStore(expected,argv[3],argv[4]);
    }
    if(argc==5&&std::strcmp(argv[1],"--finish-image-store")==0)
        return FinishImageStore();
    if(argc==4&&std::strcmp(argv[1],"--rescan")==0) {
        const uint64_t expected=std::strtoull(argv[2],nullptr,10);
        const int fd=open(argv[3],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)return HelperFail("open",argv[3],errno);
        uint64_t actual=0;
        if(!expected||ioctl(fd,BLKGETDISKSEQ,&actual)!=0||actual!=expected){CLOSE_FD(fd);return HelperFail("disk sequence changed for",argv[3],0);}
        if(ioctl(fd,BLKRRPART)!=0){const int err=errno;CLOSE_FD(fd);return HelperFail("BLKRRPART",argv[3],err);}
        CLOSE_FD(fd);
        return WaitForPartitionNodes(argv[3])?0:1;
    }
    if(argc==4&&std::strcmp(argv[1],"--health")==0)
        return PrintDriveHealth(std::strtoull(argv[2],nullptr,10),argv[3]);
    if(argc==4&&std::strcmp(argv[1],"--discard")==0)
        return DiscardDisk(std::strtoull(argv[2],nullptr,10),argv[3]);
    if(argc==5&&std::strcmp(argv[1],"--boot-entry")==0)
        return WriteEfiBootEntry(std::strtoull(argv[2],nullptr,10),argv[3],argv[4]);
    if(argc<5||std::strcmp(argv[1],"--exec-helper")!=0)return -1;
    const uint64_t expected=std::strtoull(argv[2],nullptr,10);
    const std::string command=argv[3];
    if(command!="/system/bin/sgdisk"&&command!="/system/bin/mke2fs"&&command!="/system/bin/make_f2fs") {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"storage helper: refusing unlisted tool %s",command.c_str());
        return 126;
    }
    // Keep the verified block fd open across exec and make the tool address
    // that exact kernel device object instead of reopening a reused node.
    const int target=open(argv[argc-1],O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(target<0)return HelperFail("open",argv[argc-1],errno);
    uint64_t actual=0;struct stat st{};
    if(!expected||fstat(target,&st)!=0||!S_ISBLK(st.st_mode)||
       ioctl(target,BLKGETDISKSEQ,&actual)!=0||actual!=expected)return HelperFail("not the verified block device:",argv[argc-1],0);
    if(dup2(target,198)<0)return HelperFail("dup2",argv[argc-1],errno);
    if(fcntl(198,F_SETFD,0)<0)return HelperFail("fcntl",argv[argc-1],errno);
    if(target!=198)CLOSE_FD(target);
    argv[argc-1]=const_cast<char*>("/proc/self/fd/198");
    execv(command.c_str(),argv+3);
    __android_log_print(ANDROID_LOG_ERROR,kTag,"storage helper: exec %s: %s",command.c_str(),strerror(errno));
    return 127;
}

int main(int argc,char** argv) {
    if(argc>1) {
        const int result=HelperMain(argc,argv);
        if(result>=0)return result;
    }
    __android_log_print(ANDROID_LOG_INFO,kTag,"Starting non-blocking live-only install channel service");
    g_server=matonos_ipc_create("install");
    if (!g_server || matonos_ipc_register(g_server,"get_status",GetStatus,nullptr) ||
        matonos_ipc_register(g_server,"list_drives",ListDrives,nullptr) ||
        matonos_ipc_register(g_server,"execute_operation",Execute,nullptr) ||
        matonos_ipc_register(g_server,"cancel_operation",Cancel,nullptr) ||
        matonos_ipc_register_ota(g_server,StageUpdate,FinalizeUpdate,UpdateStatus,nullptr) ||
        matonos_ipc_start(g_server)) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"Unable to register installer channel; exiting without delaying boot");
        return 1;
    }
    for (;;) std::this_thread::sleep_for(std::chrono::hours(24));
}
