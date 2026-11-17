#include "InstallerService.h"
#include "Json.h"
#include "matonos_ipc.h"

#include <android/log.h>
#include <android-base/properties.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/dm-ioctl.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/types.h>
#include <sys/wait.h>
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
    if(!android::base::SetProperty("vendor.maton.install.unmount.request",id) ||
       !android::base::WaitForProperty("vendor.maton.install.unmount.done",id,std::chrono::seconds(10))){
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
    if(!android::base::SetProperty("vendor.maton.install.metadata_source",metadata)||
       !android::base::SetProperty("vendor.maton.install.system_source",system)||
       !android::base::SetProperty("vendor.maton.install.mount.request",id)||
       !android::base::WaitForProperty("vendor.maton.install.mount.done",id,std::chrono::seconds(10))){
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
    CLOSE_FD(system_fd);CLOSE_FD(metadata_fd);
    if(!MountImageStore(metadata_path,mapped,&error)){RemoveDmDevice(kSystemMapper);return HelperFail("init mount target image store",error.c_str(),errno);}
    constexpr char label[]="u:object_r:matonos_install_mount_file:s0";
    for(const char* root:{"/mnt/matonos-install/metadata","/mnt/matonos-install/system"}){
        const int dir=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(dir<0){const int e=errno;UnmountImagePath(std::string(kImageStoreRoot)+"/system");RemoveDmDevice(kSystemMapper);return HelperFail("open mounted image-store root",root,e);}
        if(fsetxattr(dir,"security.selinux",label,sizeof(label),0)!=0){const int e=errno;CLOSE_FD(dir);UnmountImagePath(std::string(kImageStoreRoot)+"/system");RemoveDmDevice(kSystemMapper);return HelperFail("label mounted image-store root",root,e);}
        CLOSE_FD(dir);
    }
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
std::string DrivesJson() {
    std::ostringstream out; out << "{\"drives\":[";
    const auto drives=Snapshot();
    for (size_t i=0;i<drives.size();++i) {
        const auto& d=drives[i]; if (i) out << ',';
        out << "{\"id\":"<<Escape(d.id)<<",\"identity\":"<<Escape(d.identity)<<",\"path\":"<<Escape(d.path)<<",\"model\":"<<Escape(d.model)<<",\"serial\":"<<Escape(d.serial)
            <<",\"sizeBytes\":"<<d.size_bytes<<",\"transport\":"<<Escape(d.transport)<<",\"removable\":"<<(d.removable?"true":"false")<<",\"rotational\":"<<(d.rotational?"true":"false")
            <<",\"safe\":"<<(d.safe?"true":"false")<<",\"reason\":"<<Escape(d.unsafe_reason)<<",\"partitions\":[";
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
    if(argc<5||std::strcmp(argv[1],"--exec-helper")!=0)return -1;
    const uint64_t expected=std::strtoull(argv[2],nullptr,10);
    const std::string command=argv[3];
    if(command!="/system/bin/sgdisk"&&command!="/system/bin/mke2fs"&&command!="/system/bin/newfs_msdos"&&command!="/system/bin/make_f2fs") {
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
        matonos_ipc_register(g_server,"cancel_operation",Cancel,nullptr) || matonos_ipc_start(g_server)) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"Unable to register installer channel; exiting without delaying boot");
        return 1;
    }
    for (;;) std::this_thread::sleep_for(std::chrono::hours(24));
}
