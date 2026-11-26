#include "InstallerService.h"
#include "LpMetadata.h"
#include "FatFilesystem.h"
#include "Props.h"
#include "Json.h"

#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <selinux/selinux.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#define CLOSE_FD(fd_value) do { \
    const int close_fd_value = (fd_value); \
    if (close_fd_value >= 0) { \
        std::fprintf(stderr, "installer: close fd=%d at %s:%d\n", close_fd_value, __FILE__, __LINE__); \
        if (::close(close_fd_value) != 0) std::fprintf(stderr, "installer: close fd=%d failed errno=%d at %s:%d\n", close_fd_value, errno, __FILE__, __LINE__); \
    } \
} while (0)

namespace matonos::install {
namespace fs = std::filesystem;
namespace {

bool Run(const std::vector<std::string>& arguments, uint64_t disk_sequence, std::string* error) {
    if (arguments.empty()) { *error="Empty helper invocation."; return false; }
    const pid_t pid=fork();
    if (pid<0) { *error="Unable to start storage helper."; return false; }
    if (pid==0) {
        std::vector<std::string> helper_args{"/odm/bin/matonos-installer-service"};
        if(arguments[0]=="@rescan") {
            if(arguments.size()!=2)_exit(125);
            helper_args.push_back("--rescan");helper_args.push_back(std::to_string(disk_sequence));helper_args.push_back(arguments[1]);
        } else if (arguments[0] == "@discard") {
            if (arguments.size() != 2) _exit(125);
            helper_args.push_back("--discard");helper_args.push_back(std::to_string(disk_sequence));helper_args.push_back(arguments[1]);
        } else if (arguments[0] == "@boot-entry") {
            if (arguments.size() != 3) _exit(125);
            helper_args.push_back("--boot-entry");helper_args.push_back(std::to_string(disk_sequence));
            helper_args.push_back(arguments[1]);helper_args.push_back(arguments[2]);
        } else if (arguments[0] == "@prepare-image-store" || arguments[0] == "@finish-image-store") {
            if (arguments.size() != 3) _exit(125);
            helper_args.push_back(arguments[0] == "@prepare-image-store" ? "--prepare-image-store" : "--finish-image-store");
            helper_args.push_back(std::to_string(disk_sequence));
            helper_args.push_back(arguments[1]);
            helper_args.push_back(arguments[2]);
        } else {
            helper_args.push_back("--exec-helper");
            helper_args.push_back(std::to_string(disk_sequence));
            helper_args.insert(helper_args.end(),arguments.begin(),arguments.end());
        }
        std::vector<char*> argv; argv.reserve(helper_args.size()+1);
        for (const auto& arg:helper_args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(argv[0],argv.data());
        _exit(127);
    }
    int status=0;
    while (waitpid(pid,&status,0)<0) { if(errno==EINTR) continue; *error="Unable to wait for storage helper."; return false; }
    if(!WIFEXITED(status)) { *error="Storage helper did not exit normally: "+arguments[0]; return false; }
    if(WEXITSTATUS(status)!=0) { *error="Storage helper failed: "+arguments[0]+" (exit "+std::to_string(WEXITSTATUS(status))+")"; return false; }
    return true;
}

const Partition* Find(const Drive& d,const PartitionRef& r) {
    if (r.part_guid.empty() == r.logical_name.empty()) return nullptr;
    auto it=std::find_if(d.partitions.begin(),d.partitions.end(),[&](const Partition& p){
        return r.logical_name.empty()?(!p.logical&&p.part_guid==r.part_guid):(p.logical&&p.name==r.logical_name);
    });
    return it==d.partitions.end()?nullptr:&*it;
}

std::string PartitionPath(const Drive& drive,const PartitionRef& ref) {
    const auto* part=Find(drive,ref);if(!part)return {};
    if(part->logical) {
        const auto super=std::find_if(drive.partitions.begin(),drive.partitions.end(),[&](const Partition& p){return !p.logical&&p.part_guid==part->backing_part_guid;});
        return super==drive.partitions.end()?std::string():super->path;
    }
    return part->path;
}

int OpenVerifiedTarget(const std::string& path, int flags, uint64_t disk_sequence);
bool RunFiemap(const std::string& command, const std::string& name, uint64_t size,
               std::string* output, std::string* error) {
    const std::string id=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string request=id+"\n"+command+"\n"+name+"\n"+std::to_string(size)+"\n";
    const char* request_path="/mnt/matonos-install/system/.matonos-fiemap-request";
    const int request_fd=open(request_path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(request_fd<0){*error="Cannot open fiemap request file: "+std::string(strerror(errno));return false;}
    size_t written=0;
    while(written<request.size()){
        const ssize_t n=write(request_fd,request.data()+written,request.size()-written);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){const int e=errno;CLOSE_FD(request_fd);*error="Cannot write fiemap request: "+std::string(strerror(e));return false;}
        written+=static_cast<size_t>(n);
    }
    if(fsync(request_fd)!=0){const int e=errno;CLOSE_FD(request_fd);*error="Cannot sync fiemap request: "+std::string(strerror(e));return false;}
    CLOSE_FD(request_fd);
    if(!matonos::props::Set("vendor.maton.install.fiemap.request",id)){
        *error="Cannot set fiemap request property for "+id;return false;
    }
    matonos::props::WaitForInit("vendor.maton.install.fiemap.done",id,"fiemap");
    std::ifstream response("/mnt/matonos-install/system/.matonos-fiemap-result");
    std::string result_id,status_text,result;std::getline(response,result_id);std::getline(response,status_text);
    if(!response||result_id!=id){*error="Missing or stale fiemap result for request "+id;return false;}
    std::ostringstream rest;rest<<response.rdbuf();result=rest.str();
    if(status_text!="0"){*error="fiemap helper "+command+" failed (status "+status_text+")";return false;}
    if(output)*output=std::move(result);
    return true;
}

// ueventd publishes /dev/block/mapper/<name> as a symlink to the dm node.
// Resolve it once and accept only a /dev/block/dm-N block device, so the
// O_NOFOLLOW opens below still never follow anything else.
std::string ResolveDmNode(const std::string& path) {
    struct stat st{};
    if(lstat(path.c_str(),&st)!=0)return "";
    if(!S_ISLNK(st.st_mode))return S_ISBLK(st.st_mode)?path:"";
    std::error_code ec;const std::string real=fs::canonical(path,ec).string();
    if(ec||real.rfind("/dev/block/dm-",0)!=0||real.size()==14)return "";
    for(size_t i=14;i<real.size();++i)if(real[i]<'0'||real[i]>'9')return "";
    if(stat(real.c_str(),&st)!=0||!S_ISBLK(st.st_mode))return "";
    return real;
}

bool CopyToMappedImage(const std::string& source,const std::string& target,uint64_t bytes,
                       ProgressCallback progress,CancelCallback cancelled,std::string* error) {
    const std::string source_node=ResolveDmNode(source),target_node=ResolveDmNode(target);
    if(source_node.empty()||target_node.empty()){*error="Install image path is not a device-mapper node: "+(source_node.empty()?source:target);return false;}
    int in=open(source_node.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(in<0){*error="Cannot open approved live image: "+source+": "+strerror(errno);return false;}
    int out=open(target_node.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(out<0){const int e=errno;CLOSE_FD(in);*error="Cannot open mapped install image: "+target+": "+strerror(e);return false;}
    std::vector<unsigned char> buffer(4*1024*1024);uint64_t done=0;bool ok=true;
    while(done<bytes){
        if(cancelled&&cancelled()){*error="Image population cancelled between chunks.";ok=false;break;}
        const size_t want=static_cast<size_t>(std::min<uint64_t>(buffer.size(),bytes-done));size_t got_total=0;
        while(got_total<want){const ssize_t got=pread(in,buffer.data()+got_total,want-got_total,static_cast<off_t>(done+got_total));if(got<0&&errno==EINTR)continue;if(got<=0){*error="Live EROFS image ended during population.";ok=false;break;}got_total+=static_cast<size_t>(got);}
        if(!ok)break;size_t put_total=0;while(put_total<want){const ssize_t put=write(out,buffer.data()+put_total,want-put_total);if(put<0&&errno==EINTR)continue;if(put<=0){*error="Mapped EROFS image write failed: "+std::string(strerror(errno));ok=false;break;}put_total+=static_cast<size_t>(put);}
        if(!ok)break;done+=want;if(progress)progress({done,bytes,"Writing EROFS image " + std::to_string(done/bytes)});
    }
    if(ok&&fsync(out)!=0){*error="Unable to flush mapped EROFS image: "+std::string(strerror(errno));ok=false;}
    if(ok){
        uint64_t verified=0;std::vector<unsigned char> expected(buffer.size()),actual(buffer.size());
        while(verified<bytes){
            const size_t want=static_cast<size_t>(std::min<uint64_t>(buffer.size(),bytes-verified));size_t source_done=0,target_done=0;
            while(source_done<want){const ssize_t n=pread(in,expected.data()+source_done,want-source_done,static_cast<off_t>(verified+source_done));if(n<0&&errno==EINTR)continue;if(n<=0){*error="Live EROFS image readback ended early: "+std::string(strerror(errno));ok=false;break;}source_done+=static_cast<size_t>(n);}
            while(ok&&target_done<want){const ssize_t n=pread(out,actual.data()+target_done,want-target_done,static_cast<off_t>(verified+target_done));if(n<0&&errno==EINTR)continue;if(n<=0){*error="Mapped image readback ended early: "+std::string(strerror(errno));ok=false;break;}target_done+=static_cast<size_t>(n);}
            if(!ok)break;if(std::memcmp(expected.data(),actual.data(),want)!=0){*error="Mapped EROFS image readback verification failed.";ok=false;break;}
            verified+=want;if(progress)progress({verified,bytes,"Verifying EROFS image contents"});
        }
    }
    CLOSE_FD(out);CLOSE_FD(in);return ok;
}


bool Copy(const std::string& source,const std::string& target,uint64_t disk_sequence,uint64_t bytes,
          uint64_t source_bytes,uint64_t source_offset,uint64_t target_offset,
          ProgressCallback progress,CancelCallback cancelled,std::string* error) {
    int in=open(source.c_str(),O_RDONLY|O_CLOEXEC);
    if(in<0){*error="Cannot open copy source: "+source;return false;}
    int out=OpenVerifiedTarget(target,O_RDWR,disk_sequence);
    if(out<0){CLOSE_FD(in);*error="Cannot open copy target: "+target;return false;}
    std::vector<unsigned char> buffer(4*1024*1024); uint64_t done=0; bool ok=true;
    while(done<bytes) {
        if(cancelled&&cancelled()){*error="Operation cancelled between block chunks.";ok=false;break;}
        const size_t want=static_cast<size_t>(std::min<uint64_t>(buffer.size(),bytes-done));
        const size_t source_want=static_cast<size_t>(done<source_bytes?std::min<uint64_t>(want,source_bytes-done):0);
        size_t source_done=0;
        while(source_done<source_want){ssize_t got=pread(in,buffer.data()+source_done,source_want-source_done,static_cast<off_t>(source_offset+done+source_done));if(got<0&&errno==EINTR)continue;if(got<=0){*error="Copy source ended before its declared byte count.";ok=false;break;}source_done+=static_cast<size_t>(got);}
        if(!ok)break;
        std::fill(buffer.begin()+static_cast<std::ptrdiff_t>(source_want),buffer.begin()+static_cast<std::ptrdiff_t>(want),0);
        size_t at=0;
        while(at<want) {
            ssize_t put=pwrite(out,buffer.data()+at,want-at,static_cast<off_t>(target_offset+done+at));
            if(put<0&&errno==EINTR)continue;
            if(put<=0){*error="Block-device write failed.";ok=false;break;}
            at+=static_cast<size_t>(put);
        }
        if(!ok)break;
        done+=want;
        if(progress)progress({done,bytes,"Copying block data"});
    }
    if(ok&&fsync(out)!=0){*error="Unable to flush copied block data.";ok=false;}
    if(ok) {
        done=0;
        while(ok&&done<bytes) {
            if(cancelled&&cancelled()){*error="Operation cancelled during copy verification.";ok=false;break;}
            const size_t want=static_cast<size_t>(std::min<uint64_t>(buffer.size(),bytes-done));
            const size_t source_want=static_cast<size_t>(done<source_bytes?std::min<uint64_t>(want,source_bytes-done):0);
            size_t expected_done=0;
            while(expected_done<source_want){ssize_t got=pread(in,buffer.data()+expected_done,source_want-expected_done,static_cast<off_t>(source_offset+done+expected_done));if(got<0&&errno==EINTR)continue;if(got<=0){*error="Copy source readback ended early.";ok=false;break;}expected_done+=static_cast<size_t>(got);}
            if(!ok)break;
            std::fill(buffer.begin()+static_cast<std::ptrdiff_t>(source_want),buffer.begin()+static_cast<std::ptrdiff_t>(want),0);
            std::vector<unsigned char> actual(want);
            size_t at=0;
            while(at<actual.size()) {
                ssize_t got=pread(out,actual.data()+at,actual.size()-at,static_cast<off_t>(target_offset+done+at));
                if(got<0&&errno==EINTR)continue;
                if(got<=0){*error="Target readback ended before the copied extent.";ok=false;break;}
                at+=static_cast<size_t>(got);
            }
            if(!ok)break;
            if(std::memcmp(buffer.data(),actual.data(),actual.size())!=0){*error="Block copy readback verification failed.";ok=false;break;}
            done+=want;
            if(progress)progress({done,bytes,"Verifying copied block data"});
        }
    }
    CLOSE_FD(out);CLOSE_FD(in);return ok;
}

uint64_t BlockBytes(int fd) {
    uint64_t bytes=0;
    if(ioctl(fd,BLKGETSIZE64,&bytes)==0)return bytes;
    const off_t end=lseek(fd,0,SEEK_END);
    if(end>=0)return static_cast<uint64_t>(end);
    return 0;
}

int OpenVerifiedTarget(const std::string& path, int flags, uint64_t disk_sequence) {
    if (disk_sequence == 0) { errno = ESTALE; return -1; }
    int fd = open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    uint64_t opened_sequence = 0;
    if (ioctl(fd, BLKGETDISKSEQ, &opened_sequence) != 0 || opened_sequence != disk_sequence) {
        CLOSE_FD(fd);
        errno = ESTALE;
        return -1;
    }
    return fd;
}

uint32_t LoadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t LoadLe64(const uint8_t* data) {
    return static_cast<uint64_t>(LoadLe32(data)) |
           (static_cast<uint64_t>(LoadLe32(data + 4)) << 32);
}

uint32_t GptCrc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

std::string GptGuid(const uint8_t* bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    static constexpr uint8_t kOrder[] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < std::size(kOrder); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        const uint8_t value = bytes[kOrder[i]];
        out.push_back(kHex[value >> 4]);
        out.push_back(kHex[value & 0x0f]);
    }
    return out;
}

bool ReadExactAt(int fd, void* output, size_t size, uint64_t offset) {
    auto* bytes = static_cast<uint8_t*>(output);
    size_t done = 0;
    while (done < size) {
        const ssize_t got = pread(fd, bytes + done, size - done,
                                  static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += static_cast<size_t>(got);
    }
    return true;
}

bool ReadVerifiedGptType(const Drive& drive, const Partition& partition,
                         std::string* type_guid, std::string* error) {
    const int fd = OpenVerifiedTarget(drive.path, O_RDONLY, drive.disk_sequence);
    if (fd < 0) { *error = "Cannot open the selected GPT for readback verification."; return false; }
    auto fail = [&](const char* message) { CLOSE_FD(fd); *error = message; return false; };

    int sector_size = 0;
    if (ioctl(fd, BLKSSZGET, &sector_size) != 0 || sector_size < 512 ||
        sector_size > 65536 || (sector_size & (sector_size - 1)) != 0)
        return fail("Cannot verify the selected disk's logical sector size.");
    const uint64_t disk_bytes = BlockBytes(fd);
    if (disk_bytes < static_cast<uint64_t>(sector_size) * 3 ||
        disk_bytes % static_cast<uint64_t>(sector_size) != 0)
        return fail("Selected disk has an invalid size for GPT verification.");

    std::vector<uint8_t> sector(static_cast<size_t>(sector_size));
    if (!ReadExactAt(fd, sector.data(), sector.size(), static_cast<uint64_t>(sector_size)))
        return fail("Cannot read the selected disk's primary GPT header.");
    static constexpr uint8_t kSignature[] = {'E','F','I',' ','P','A','R','T'};
    if (!std::equal(std::begin(kSignature), std::end(kSignature), sector.begin()))
        return fail("Selected disk does not contain a valid primary GPT header.");

    const uint32_t header_size = LoadLe32(sector.data() + 12);
    if (header_size < 92 || header_size > sector.size())
        return fail("Selected disk GPT header has an invalid size.");
    const uint32_t expected_header_crc = LoadLe32(sector.data() + 16);
    std::vector<uint8_t> header(sector.begin(), sector.begin() + header_size);
    std::fill(header.begin() + 16, header.begin() + 20, 0);
    if (GptCrc32(header.data(), header.size()) != expected_header_crc || LoadLe64(sector.data() + 24) != 1)
        return fail("Selected disk GPT header CRC or current-LBA check failed.");

    const uint64_t entries_lba = LoadLe64(sector.data() + 72);
    const uint32_t entry_count = LoadLe32(sector.data() + 80);
    const uint32_t entry_size = LoadLe32(sector.data() + 84);
    const uint32_t expected_entries_crc = LoadLe32(sector.data() + 88);
    if (entries_lba < 2 || entry_count == 0 || entry_count > 4096 ||
        entry_size < 128 || entry_size > 4096 || (entry_size % 8) != 0)
        return fail("Selected disk GPT partition array has invalid dimensions.");
    const uint64_t array_bytes = static_cast<uint64_t>(entry_count) * entry_size;
    if (entries_lba > std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(sector_size))
        return fail("Selected disk GPT partition array offset overflows.");
    const uint64_t array_offset = entries_lba * static_cast<uint64_t>(sector_size);
    if (array_offset > disk_bytes || array_bytes > disk_bytes - array_offset || array_bytes > 16 * 1024 * 1024)
        return fail("Selected disk GPT partition array is outside the disk bounds.");
    std::vector<uint8_t> entries(static_cast<size_t>(array_bytes));
    if (!ReadExactAt(fd, entries.data(), entries.size(), array_offset))
        return fail("Cannot read the selected disk's GPT partition array.");
    if (GptCrc32(entries.data(), entries.size()) != expected_entries_crc)
        return fail("Selected disk GPT partition array CRC check failed.");

    for (uint32_t index = 0; index < entry_count; ++index) {
        const uint8_t* entry = entries.data() + static_cast<size_t>(index) * entry_size;
        if (GptGuid(entry + 16) != partition.part_guid) continue;
        const uint64_t first_lba = LoadLe64(entry + 32);
        const uint64_t last_lba = LoadLe64(entry + 40);
        if (first_lba > last_lba || last_lba >= disk_bytes / static_cast<uint64_t>(sector_size) ||
            first_lba * static_cast<uint64_t>(sector_size) != partition.start_bytes ||
            (last_lba - first_lba + 1) * static_cast<uint64_t>(sector_size) != partition.size_bytes)
            return fail("Selected partition bounds do not match its verified GPT entry.");
        *type_guid = GptGuid(entry);
        CLOSE_FD(fd);
        return true;
    }
    return fail("Selected partition GUID is absent from the verified GPT.");
}

bool WriteGptToDisk(const WriteGpt& gpt,const Drive& d,const ProgressCallback& progress,std::string* error) {
    // The user already confirmed erasing this drive: trim it so the SSD starts
    // the new layout with every block free. Unsupported drives just log.
    if(progress)progress({0,0,"Trimming drive"});
    if(!Run({"@discard",d.path},d.disk_sequence,error))return false;
    if(progress)progress({0,0,"Writing partition table"});
    std::vector<std::string> args{"/system/bin/sgdisk","--zap-all","--clear","--disk-guid="+gpt.disk_guid};
    for(size_t i=0;i<gpt.partitions.size();++i) {
        const auto& p=gpt.partitions[i];
        const uint64_t first=p.start_bytes/512;
        const uint64_t sectors=p.size_bytes/512;
        const uint64_t last=first+sectors-1;
        const std::string n=std::to_string(i+1);
        args.push_back("--new="+n+":"+std::to_string(first)+":"+std::to_string(last));
        args.push_back("--typecode="+n+":"+p.type_guid);
        args.push_back("--change-name="+n+":"+p.name);
        args.push_back("--partition-guid="+n+":"+p.part_guid);
    }
    args.push_back(d.path);
    if(!Run(args,d.disk_sequence,error))return false;
    if(!Run({"@rescan",d.path},d.disk_sequence,error))return false;
    return true;
}

std::string LiveEspPath() {
    std::ifstream input("/proc/cmdline");std::string line,word,uuid;
    if(!std::getline(input,line))return {};
    std::istringstream words(line);
    while(words>>word)if(word.rfind("androidboot.boot_part_uuid=",0)==0)uuid=word.substr(27);
    if(uuid.empty())return {};
    std::error_code ec;
    for(const auto& disk:fs::directory_iterator("/sys/block",ec)) {
        if(ec)break;
        for(const auto& child:fs::directory_iterator(disk.path(),ec)) {
            if(ec)break;
            if(!fs::exists(child.path()/"partition",ec))continue;
            std::ifstream uevent(child.path()/"uevent");std::string field;
            while(std::getline(uevent,field))if(field=="PARTUUID="+uuid) {
                const auto node=fs::path("/dev/block")/child.path().filename();
                return fs::exists(node)?node.string():std::string();
            }
        }
    }
    return {};
}

std::string LivePayloadPath(const std::string& id) {
    if(id=="live/esp/EFI/BOOT/BOOTX64.EFI")return "EFI/BOOT/BOOTX64.EFI";
    if(id=="live/esp/EFI/BOOT/grubx64.efi")return "EFI/BOOT/grubx64.efi";
    if(id=="live/esp/EFI/BOOT/mmx64.efi")return "EFI/BOOT/mmx64.efi";
    if(id=="live/esp/EFI/BOOT/matonos-dev.der")return "EFI/BOOT/matonos-dev.der";
    if(id=="live/esp/EFI/systemd/systemd-bootx64.efi")return "EFI/systemd/systemd-bootx64.efi";
    if(id=="live/esp/EFI/Linux/matonos-installed-a.efi")return "EFI/Linux/matonos-installed-a.efi";
    if(id=="live/esp/EFI/Linux/matonos-installed-a-hdd.efi")return "EFI/Linux/matonos-installed-a-hdd.efi";
    if(id=="live/esp/EFI/Linux/matonos-installed-b.efi")return "EFI/Linux/matonos-installed-b.efi";
    if(id=="live/esp/EFI/Linux/matonos-installed-b-hdd.efi")return "EFI/Linux/matonos-installed-b-hdd.efi";
    if(id=="live/boot/a/bzImage")return "android/bzImage";
    if(id=="live/boot/a/vendor_ramdisk.img")return "android/vendor_ramdisk.img";
    if(id=="live/boot/a/ramdisk.img")return "android/ramdisk.img";
    return {};
}

bool WriteFilesToPartition(const Drive& drive,const WriteFiles& op,ProgressCallback progress,
                           CancelCallback cancelled,std::string* error) {
    const auto* partition=Find(drive,op.target);
    const auto target_device=PartitionPath(drive,op.target);
    if(!partition||partition->logical||target_device.empty()){
        *error="File output target is not a verified EFI or XBOOTLDR partition.";return false;
    }
    std::string actual_type;
    if(!ReadVerifiedGptType(drive,*partition,&actual_type,error))return false;
    if(actual_type!="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"&&
       actual_type!="bc13c2ff-59e6-4262-a352-b275fd6f7172"){
        *error="File output target is not a verified EFI or XBOOTLDR partition.";return false;
    }
    std::vector<std::string> source_relatives;bool needs_live=false;
    for(const auto& file:op.files) {
        if(file.live_payload_path.empty())source_relatives.emplace_back();
        else {
            const auto relative=LivePayloadPath(file.live_payload_path);
            if(relative.empty()){*error="Live file payload is not in the fixed read-only source catalog.";return false;}
            source_relatives.push_back(relative);needs_live=true;
        }
    }
    FatVolume target_volume;
    if(!FatVolume::Open(target_device,true,&target_volume,error,drive.disk_sequence))return false;
    FatVolume source_volume;
    if(needs_live) {
        const auto source=LiveEspPath();
        if(source.empty()){*error="Live ESP identified by the boot PARTUUID is unavailable.";return false;}
        if(!FatVolume::Open(source,false,&source_volume,error))return false;
    }
    uint64_t bytes_done=0,bytes_total=0;
    for(size_t index=0;index<op.files.size();++index) {
        if(cancelled&&cancelled()){*error="File write cancelled between files.";return false;}
        const auto& file=op.files[index];
        const std::string destination=op.relative_directory.empty()?file.relative_path:op.relative_directory+"/"+file.relative_path;
        std::vector<uint8_t> contents;
        if(file.live_payload_path.empty())contents.assign(file.inline_contents.begin(),file.inline_contents.end());
        else if(!source_volume.ReadFile(source_relatives[index],&contents,error))return false;
        bytes_total+=contents.size();
        if(!target_volume.WriteFile(destination,contents,error))return false;
        bytes_done+=contents.size();
        if(progress)progress({bytes_done,bytes_total,"Writing approved boot payloads"});
    }
    return true;
}



}  // namespace

bool ExecutePrimitive(const Operation& operation,const Drive& target,ProgressCallback progress,
                      CancelCallback cancelled,std::string* error) {
    if(cancelled&&cancelled()){*error="Operation cancelled before execution.";return false;}
    return std::visit([&](const auto& op)->bool {
        using T=std::decay_t<decltype(op)>;
        if constexpr(std::is_same_v<T,WriteGpt>) return WriteGptToDisk(op,target,progress,error);
        else if constexpr(std::is_same_v<T,CreateLpMetadata>) {
            const auto super=std::find_if(target.partitions.begin(),target.partitions.end(),[&](const Partition& p){return !p.logical&&p.part_guid==op.super_part_guid;});
            if(super==target.partitions.end()||super->path.empty()){*error="Selected super partition disappeared before LP metadata creation.";return false;}
            return WriteLpMetadata(super->path,super->size_bytes,super->name,op,target.disk_sequence,error);
        } else if constexpr(std::is_same_v<T,PopulateImages>) {
            const auto system=std::find_if(target.partitions.begin(),target.partitions.end(),[&](const Partition& p){return !p.logical&&p.part_guid==op.maton_system_part_guid;});
            const auto metadata=std::find_if(target.partitions.begin(),target.partitions.end(),[&](const Partition& p){return !p.logical&&p.part_guid==op.metadata_part_guid;});
            if(system==target.partitions.end()||metadata==target.partitions.end()){*error="Image-store partitions disappeared after validation.";return false;}
            if(!Run({"@prepare-image-store",system->path,metadata->path},target.disk_sequence,error))return false;
            bool ok=true;
            for(size_t index=0;index<op.images.size()&&ok;++index){
                if(cancelled&&cancelled()){*error="Image population cancelled between partitions.";ok=false;break;}
                const auto& image=op.images[index];const std::string source="/dev/block/mapper/"+image.live_partition_name;
                const std::string source_node=ResolveDmNode(source);
                int source_fd=source_node.empty()?-1:open(source_node.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
                if(source_node.empty())errno=ENODEV;
                if(source_fd<0){*error="Approved live partition unavailable: "+source+": "+strerror(errno);ok=false;break;}
                const uint64_t size=BlockBytes(source_fd);CLOSE_FD(source_fd);
                if(size==0){*error="Cannot determine the live partition image size: "+source;ok=false;break;}
                if(!RunFiemap("create",image.name,size,nullptr,error)){ok=false;break;}
                std::string mapped;
                if(!RunFiemap("map",image.name,0,&mapped,error)){ok=false;RunFiemap("unmap",image.name,0,nullptr,error);break;}
                while(!mapped.empty()&&(mapped.back()=='\n'||mapped.back()=='\r'))mapped.pop_back();
                if(mapped.empty()||mapped.front()!='/'){*error="fiemap helper returned an invalid mapped device path.";ok=false;}
                else ok=CopyToMappedImage(source,mapped,size,progress,cancelled,error);
                std::string unmap_error;
                if(!RunFiemap("unmap",image.name,0,nullptr,&unmap_error)){if(ok){*error=unmap_error;ok=false;}}
            }
            // Persist the store's GPT name for first-stage init (see helper).
            if(ok&&!RunFiemap("finalize","maton_system",0,nullptr,error))ok=false;
            std::string finish_error;
            if(!Run({"@finish-image-store",system->path,metadata->path},target.disk_sequence,&finish_error)){if(ok){*error=finish_error;ok=false;}}
            return ok;
        } else if constexpr(std::is_same_v<T,WriteSetup>) {
            std::fprintf(stderr,"installer: write_setup start part=%s\n",op.maton_system_part_guid.c_str());
            const auto system=std::find_if(target.partitions.begin(),target.partitions.end(),[&](const Partition& p){return !p.logical&&p.part_guid==op.maton_system_part_guid;});
            const auto metadata=std::find_if(target.partitions.begin(),target.partitions.end(),[](const Partition& p){return !p.logical&&p.name=="metadata";});
            if(system==target.partitions.end()||metadata==target.partitions.end()){*error="Setup write target image-store partitions disappeared.";return false;}
            matonos::install::json::Value setup;
            if(!matonos::install::json::Parse(op.setup_json,&setup)||setup.kind!=matonos::install::json::Value::Kind::kObject||!setup.Get("version")||setup.Get("version")->Number(UINT64_MAX)!=1){*error="Setup payload is not a JSON object with version 1.";return false;}
            if(!Run({"@prepare-image-store",system->path,metadata->path},target.disk_sequence,error))return false;
            bool ok=true; const std::string directory="/mnt/matonos-install/system/setup";
            if(lsetfilecon("/mnt/matonos-install/system","u:object_r:matonos_install_mount_file:s0")!=0){*error="Cannot label image-store root for the installed bridge: "+std::string(strerror(errno));ok=false;}
            std::error_code ec; if(ok)fs::create_directories(directory,ec);
            if(ok&&ec){*error="Cannot create setup directory: "+ec.message();ok=false;}
            // The bridge UID is assigned on first boot, so use open DAC modes and let SELinux gate access.
            if(ok&&chmod(directory.c_str(),0777)!=0){*error="Cannot set setup directory mode: "+std::string(strerror(errno));ok=false;}
            if(ok&&lsetfilecon(directory.c_str(),"u:object_r:matonos_setup_file:s0")!=0){*error="Cannot label setup directory: "+std::string(strerror(errno));ok=false;}
            const std::string temporary=directory+"/setup.json.tmp", final=directory+"/setup.json";
            int fd=-1;
            if(ok){fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0666);if(fd<0){*error="Cannot create setup temporary file: "+std::string(strerror(errno));ok=false;}}
            if(ok&&fchmod(fd,0666)!=0){*error="Cannot set setup file mode: "+std::string(strerror(errno));ok=false;}
            if(ok&&fsetfilecon(fd,"u:object_r:matonos_setup_file:s0")!=0){*error="Cannot label setup file: "+std::string(strerror(errno));ok=false;}
            size_t written=0;
            while(ok&&written<op.setup_json.size()){ssize_t n=write(fd,op.setup_json.data()+written,op.setup_json.size()-written);if(n<0&&errno==EINTR)continue;if(n<=0){*error="Cannot write setup file: "+std::string(strerror(errno));ok=false;break;}written+=static_cast<size_t>(n);}
            if(ok&&fsync(fd)!=0){*error="Cannot sync setup file: "+std::string(strerror(errno));ok=false;}
            if(fd>=0)CLOSE_FD(fd);
            if(ok&&rename(temporary.c_str(),final.c_str())!=0){*error="Cannot publish setup file: "+std::string(strerror(errno));ok=false;}
            if(ok){int dirfd=open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dirfd<0){*error="Cannot open setup directory for sync: "+std::string(strerror(errno));ok=false;}else{if(fsync(dirfd)!=0){*error="Cannot sync setup directory: "+std::string(strerror(errno));ok=false;}CLOSE_FD(dirfd);}}
            std::string finish_error;if(!Run({"@finish-image-store",system->path,metadata->path},target.disk_sequence,&finish_error)&&ok){*error=finish_error;ok=false;}
            std::fprintf(stderr,"installer: write_setup %s part=%s%s%s\n",ok?"finished":"failed",op.maton_system_part_guid.c_str(),ok?"":" error=",ok?"":error->c_str());
            return ok;
        } else if constexpr(std::is_same_v<T,Format>) {
            const auto path=PartitionPath(target,op.target); if(path.empty()){*error="Format target disappeared after safety validation.";return false;}
            const auto* part=Find(target,op.target);if(!part||part->logical){*error="Filesystem format is supported only on a physical GPT partition.";return false;}
            if(op.filesystem==Filesystem::kExt4) return Run({"/system/bin/mke2fs","-t","ext4","-E","nodiscard","-F","-L",op.label,path},target.disk_sequence,error);
            if(op.filesystem==Filesystem::kVfat) return FatVolume::Format(path,op.label,error,target.disk_sequence);
            if(op.filesystem==Filesystem::kF2fs) return Run({"/system/bin/make_f2fs","-f","-t","0","-g","android","-O","encrypt,quota,verity","-l",op.label,path},target.disk_sequence,error);
            *error="Unknown filesystem in format operation.";return false;
        } else if constexpr(std::is_same_v<T,CopyPartition>) {
            const auto* dst=Find(target,op.target); const auto dest_path=PartitionPath(target,op.target);
            if(!dst||dest_path.empty()){*error="Copy target disappeared after safety validation.";return false;}
            std::string src_path;
            if(op.source_kind==CopySourceKind::kTargetPartition) src_path=PartitionPath(target,op.source);
            else {
                // Fixed live source catalog: logical names only, no caller path.
                static const std::vector<std::string> allowed{"system_a","system_ext_a","product_a","vendor_a","odm_a","vbmeta_a","vbmeta_b","system","system_ext","product","vendor","odm"};
                if(std::find(allowed.begin(),allowed.end(),op.live_partition_name)==allowed.end()){*error="Live image payload is not in the service catalog.";return false;}
                src_path=(fs::path("/dev/block/mapper")/op.live_partition_name).string();
                if(!fs::exists(src_path)&&op.live_partition_name.size()>2&&op.live_partition_name.substr(op.live_partition_name.size()-2)=="_a")
                    src_path=(fs::path("/dev/block/mapper")/op.live_partition_name.substr(0,op.live_partition_name.size()-2)).string();
            }
            const auto* src=op.source_kind==CopySourceKind::kTargetPartition?Find(target,op.source):nullptr;
            uint64_t bytes=src?src->size_bytes:dst->size_bytes;
            if(src&&src->size_bytes>dst->size_bytes){*error="Destination extent is smaller than source.";return false;}
            if(op.source_kind==CopySourceKind::kLiveImagePartition) {
                int source_fd=open(src_path.c_str(),O_RDONLY|O_CLOEXEC);
                if(source_fd<0){*error="The approved live image source is unavailable.";return false;}
                const uint64_t source_bytes=BlockBytes(source_fd);CLOSE_FD(source_fd);
                if(source_bytes==0||source_bytes>dst->size_bytes){*error="Live image source size cannot be verified or exceeds its target.";return false;}
                bytes=source_bytes;
            }
            const uint64_t source_offset=src&&src->logical?src->start_bytes:0;
            const uint64_t target_offset=dst->logical?dst->start_bytes:0;
            return Copy(src_path,dest_path,target.disk_sequence,dst->size_bytes,bytes,source_offset,target_offset,std::move(progress),std::move(cancelled),error);
        } else if constexpr(std::is_same_v<T,ClonePartition>) {
            const auto* src=Find(target,op.source);const auto* dst=Find(target,op.target);
            const auto source=PartitionPath(target,op.source),dest=PartitionPath(target,op.target);
            if(!src||!dst||source.empty()||dest.empty()){*error="Clone partition disappeared after safety validation.";return false;}
            if(src->size_bytes>dst->size_bytes){*error="Clone destination is smaller than source.";return false;}
            return Copy(source,dest,target.disk_sequence,src->size_bytes,src->size_bytes,src->logical?src->start_bytes:0,dst->logical?dst->start_bytes:0,std::move(progress),std::move(cancelled),error);
        } else if constexpr(std::is_same_v<T,WriteFiles>) {
            return WriteFilesToPartition(target,op,std::move(progress),std::move(cancelled),error);
        } else if constexpr(std::is_same_v<T,RegisterBootEntry>) {
            const auto* esp=Find(target,op.esp);
            if(!esp||esp->logical||esp->path.empty()){*error="ESP disappeared after safety validation.";return false;}
            // The kernel's partition uevent has no type GUID: read it from the verified GPT.
            std::string type;
            if(!ReadVerifiedGptType(target,*esp,&type,error))return false;
            if(strcasecmp(type.c_str(),"c12a7328-f81f-11d2-ba4b-00a0c93ec93b")!=0){*error="Boot entry target is not an EFI system partition.";return false;}
            if(progress)progress({0,0,"Adding firmware boot entry"});
            return Run({"@boot-entry",esp->path,esp->part_guid},target.disk_sequence,error);
        }
        *error="Unsupported operation."; return false;
    },operation);
}

}  // namespace matonos::install
