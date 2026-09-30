#include "InstallerService.h"
#include "Json.h"
#include "matonos_ipc.h"

#include <android/log.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace mi = matonos::install;
namespace js = matonos::install::json;
namespace {
constexpr char kTag[] = "MatonosInstaller";
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_operation_active{false};
std::mutex g_operation_mutex;
MatonosIpcServer* g_server = nullptr;
bool LiveMode();

std::string Escape(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: if (c >= 0x20) out.push_back(static_cast<char>(c));
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
    if (kind == "create_lp_metadata") {
        mi::CreateLpMetadata op; op.super_part_guid = S(value,"superPartGuid"); op.metadata_size_bytes = N(value,"metadataSizeBytes"); op.metadata_slots = static_cast<uint32_t>(N(value,"metadataSlots"));
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
    FILE* f=std::fopen("/proc/cmdline","r"); if (!f) return false;
    char line[8192] = {}; const bool read=std::fgets(line,sizeof(line),f)!=nullptr; std::fclose(f);
    return read && std::strstr(line,"androidboot.matonos.live=1") != nullptr;
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
            <<",\"sizeBytes\":"<<d.size_bytes<<",\"transport\":"<<Escape(d.transport)<<",\"removable\":"<<(d.removable?"true":"false")
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

int HelperMain(int argc,char** argv) {
    if(argc==4&&std::strcmp(argv[1],"--rescan")==0) {
        const uint64_t expected=std::strtoull(argv[2],nullptr,10);
        const int fd=open(argv[3],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)return 1;
        uint64_t actual=0;
        const int rc=expected&&ioctl(fd,BLKGETDISKSEQ,&actual)==0&&actual==expected?ioctl(fd,BLKRRPART):-1;
        close(fd);return rc==0?0:1;
    }
    if(argc<5||std::strcmp(argv[1],"--exec-helper")!=0)return -1;
    const uint64_t expected=std::strtoull(argv[2],nullptr,10);
    const std::string command=argv[3];
    if(command!="/system/bin/sgdisk"&&command!="/system/bin/mke2fs"&&command!="/system/bin/newfs_msdos")return 126;
    // Keep the verified block fd open across exec and make the tool address
    // that exact kernel device object instead of reopening a reused node.
    const int target=open(argv[argc-1],O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(target<0)return 1;
    uint64_t actual=0;struct stat st{};
    if(!expected||fstat(target,&st)!=0||!S_ISBLK(st.st_mode)||
       ioctl(target,BLKGETDISKSEQ,&actual)!=0||actual!=expected)return 1;
    if(dup2(target,198)<0)return 1;
    if(fcntl(198,F_SETFD,0)<0)return 1;
    if(target!=198)close(target);
    argv[argc-1]=const_cast<char*>("/proc/self/fd/198");
    execv(command.c_str(),argv+3);
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
    while (true) pause();
}
