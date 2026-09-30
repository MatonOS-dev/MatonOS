#include "FatFilesystem.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace matonos::install {
namespace {
constexpr uint32_t kEoc=0x0fffffff;
constexpr uint64_t kMaxFat32Clusters=0x0ffffff5ULL;
uint16_t U16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint32_t U32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void P16(uint8_t* p,uint16_t x){p[0]=uint8_t(x);p[1]=uint8_t(x>>8);}
void P32(uint8_t* p,uint32_t x){p[0]=uint8_t(x);p[1]=uint8_t(x>>8);p[2]=uint8_t(x>>16);p[3]=uint8_t(x>>24);}
bool ReadAt(int fd,void* data,size_t size,uint64_t off){size_t done=0;while(done<size){ssize_t n=pread(fd,static_cast<uint8_t*>(data)+done,size-done,off+done);if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=static_cast<size_t>(n);}return true;}
bool WriteAt(int fd,const void* data,size_t size,uint64_t off){size_t done=0;while(done<size){ssize_t n=pwrite(fd,static_cast<const uint8_t*>(data)+done,size-done,off+done);if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=static_cast<size_t>(n);}return true;}
std::string Upper(std::string s){for(char& c:s)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return s;}
std::string DecodeShort(const uint8_t* e){std::string base(reinterpret_cast<const char*>(e),8),ext(reinterpret_cast<const char*>(e+8),3);while(!base.empty()&&base.back()==' ')base.pop_back();while(!ext.empty()&&ext.back()==' ')ext.pop_back();if(base.size()>0&&static_cast<uint8_t>(base[0])==0x05)base[0]=static_cast<char>(0xe5);return ext.empty()?base:base+"."+ext;}
uint8_t Checksum(const uint8_t* short_name){uint8_t sum=0;for(int i=0;i<11;++i)sum=static_cast<uint8_t>(((sum&1)?0x80:0)+(sum>>1)+short_name[i]);return sum;}
std::vector<std::string> Components(const std::string& path){std::vector<std::string> out;std::string part;for(char c:path){if(c=='/'){if(!part.empty()){out.push_back(part);part.clear();}}else part.push_back(c);}if(!part.empty())out.push_back(part);return out;}
}  // namespace

FatVolume::~FatVolume(){if(fd_>=0)close(fd_);}
FatVolume::FatVolume(FatVolume&& other) noexcept {*this=std::move(other);}
FatVolume& FatVolume::operator=(FatVolume&& other) noexcept {
    if(this==&other)return *this;
    if(fd_>=0)close(fd_);
    fd_=other.fd_;other.fd_=-1;writable_=other.writable_;bytes_per_sector_=other.bytes_per_sector_;sectors_per_cluster_=other.sectors_per_cluster_;reserved_sectors_=other.reserved_sectors_;fat_count_=other.fat_count_;sectors_per_fat_=other.sectors_per_fat_;root_cluster_=other.root_cluster_;total_sectors_=other.total_sectors_;fat_offset_=other.fat_offset_;data_offset_=other.data_offset_;cluster_size_=other.cluster_size_;cluster_count_=other.cluster_count_;next_free_cluster_=other.next_free_cluster_;return *this;
}

bool FatVolume::Open(const std::string& path,bool writable,FatVolume* out,std::string* error,uint64_t expected_disk_sequence){
    FatVolume volume;volume.fd_=open(path.c_str(),(writable?O_RDWR:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW);volume.writable_=writable;
    if(volume.fd_<0){*error="Unable to open FAT partition.";return false;}
    if(writable&&expected_disk_sequence){uint64_t opened_sequence=0;if(ioctl(volume.fd_,BLKGETDISKSEQ,&opened_sequence)!=0||opened_sequence!=expected_disk_sequence){*error="Selected disk changed before opening its FAT partition.";return false;}}
    uint8_t b[512];if(!ReadAt(volume.fd_,b,sizeof(b),0)){*error="Unable to read FAT boot sector.";return false;}
    volume.bytes_per_sector_=U16(b+11);volume.sectors_per_cluster_=b[13];volume.reserved_sectors_=U16(b+14);volume.fat_count_=b[16];
    volume.total_sectors_=U32(b+32);volume.sectors_per_fat_=U32(b+36);volume.root_cluster_=U32(b+44);
    if(volume.bytes_per_sector_<512||volume.bytes_per_sector_>4096||(volume.bytes_per_sector_&(volume.bytes_per_sector_-1))||volume.sectors_per_cluster_==0||(volume.sectors_per_cluster_&(volume.sectors_per_cluster_-1))||volume.reserved_sectors_==0||volume.fat_count_==0||volume.fat_count_>4||volume.sectors_per_fat_==0||volume.root_cluster_<2||volume.total_sectors_==0||b[510]!=0x55||b[511]!=0xaa){*error="Target filesystem is not a valid FAT32 volume.";return false;}
    if(U16(b+17)!=0||U16(b+22)!=0||U32(b+36)==0){*error="Only FAT32 EFI filesystems are supported.";return false;}
    volume.fat_offset_=uint64_t(volume.reserved_sectors_)*volume.bytes_per_sector_;
    const uint64_t fat_sectors=uint64_t(volume.fat_count_)*volume.sectors_per_fat_;
    const uint64_t data_sector=uint64_t(volume.reserved_sectors_)+fat_sectors;
    if(data_sector>volume.total_sectors_||data_sector>UINT64_MAX/volume.bytes_per_sector_){*error="FAT volume geometry exceeds the declared volume.";return false;}
    volume.data_offset_=data_sector*volume.bytes_per_sector_;
    volume.cluster_size_=uint64_t(volume.bytes_per_sector_)*volume.sectors_per_cluster_;
    if(volume.total_sectors_<=volume.data_offset_/volume.bytes_per_sector_){*error="FAT volume has no data clusters.";return false;}
    volume.cluster_count_=(uint64_t(volume.total_sectors_)*volume.bytes_per_sector_-volume.data_offset_)/volume.cluster_size_;
    struct stat st{};if(fstat(volume.fd_,&st)!=0){*error="Unable to inspect FAT volume size.";return false;}
    uint64_t device_bytes=0;if(ioctl(volume.fd_,BLKGETSIZE64,&device_bytes)!=0)device_bytes=st.st_size>0?static_cast<uint64_t>(st.st_size):0;
    if(device_bytes<uint64_t(volume.total_sectors_)*volume.bytes_per_sector_||volume.cluster_count_<65525||volume.cluster_count_>kMaxFat32Clusters||volume.cluster_count_+2>uint64_t(volume.sectors_per_fat_)*volume.bytes_per_sector_/4){*error="FAT volume geometry exceeds its device or allocation table.";return false;}
    if(volume.root_cluster_>=volume.cluster_count_+2){*error="FAT root directory cluster is outside the volume.";return false;}
    *out=std::move(volume);return true;
}

bool FatVolume::FatEntry(uint32_t cluster,uint32_t* value){uint8_t b[4];if(!ReadAt(fd_,b,4,fat_offset_+uint64_t(cluster)*4))return false;*value=U32(b)&0x0fffffff;return true;}
bool FatVolume::SetFatEntry(uint32_t cluster,uint32_t value){if(!writable_||cluster>=cluster_count_+2)return false;uint8_t b[4];P32(b,value&0x0fffffff);for(uint32_t i=0;i<fat_count_;++i)if(!WriteAt(fd_,b,4,fat_offset_+uint64_t(i)*sectors_per_fat_*bytes_per_sector_+uint64_t(cluster)*4))return false;return true;}
bool FatVolume::ReadCluster(uint32_t cluster,std::vector<uint8_t>* bytes){if(cluster<2||cluster>=cluster_count_+2)return false;bytes->resize(cluster_size_);return ReadAt(fd_,bytes->data(),bytes->size(),data_offset_+uint64_t(cluster-2)*cluster_size_);}
bool FatVolume::WriteCluster(uint32_t cluster,const std::vector<uint8_t>& bytes){if(!writable_||cluster<2||cluster>=cluster_count_+2||bytes.size()!=cluster_size_)return false;return WriteAt(fd_,bytes.data(),bytes.size(),data_offset_+uint64_t(cluster-2)*cluster_size_);}
bool FatVolume::DirectoryChain(uint32_t cluster,std::vector<uint32_t>* chain,std::string* error){
    chain->clear();for(uint64_t i=0;i<cluster_count_;++i){if(cluster<2||cluster>=cluster_count_+2){*error="FAT directory references an invalid cluster.";return false;}if(std::find(chain->begin(),chain->end(),cluster)!=chain->end()){*error="FAT directory contains a cluster loop.";return false;}chain->push_back(cluster);uint32_t next;if(!FatEntry(cluster,&next)){*error="Unable to read FAT allocation table.";return false;}if(next>=0x0ffffff8)return true;if(next==0||next==1||next==0x0ffffff7){*error="FAT directory chain is corrupt.";return false;}cluster=next;}
    *error="FAT directory chain exceeds the volume bounds.";return false;
}
bool FatVolume::Allocate(uint32_t* cluster,std::string* error){if(!writable_){*error="FAT volume is read-only.";return false;}const uint64_t end=cluster_count_+2;for(uint64_t attempt=0;attempt<cluster_count_;++attempt){const uint32_t c=static_cast<uint32_t>(2+(uint64_t(next_free_cluster_-2)+attempt)%cluster_count_);uint32_t value;if(!FatEntry(c,&value)){*error="Unable to scan the FAT allocation table.";return false;}if(value==0){if(!SetFatEntry(c,kEoc)){*error="Unable to allocate FAT cluster.";return false;}std::vector<uint8_t> zero(cluster_size_,0);if(!WriteCluster(c,zero)){(void)SetFatEntry(c,0);*error="Unable to clear allocated FAT cluster.";return false;}next_free_cluster_=c+1<end?c+1:2;*cluster=c;return true;}}*error="Target FAT volume is out of free clusters.";return false;}

bool FatVolume::Find(const std::vector<uint32_t>& directory,const std::string& name,uint8_t* found,std::string* error,uint64_t* entry_offset){
    error->clear();std::string wanted=Upper(name);std::vector<std::array<uint16_t,13>> lfn_parts;bool lfn_active=false;uint8_t lfn_checksum=0;
    for(uint32_t cluster:directory){std::vector<uint8_t> bytes;if(!ReadCluster(cluster,&bytes)){*error="Unable to read FAT directory cluster.";return false;}
        for(size_t off=0;off+32<=bytes.size();off+=32){const uint8_t* e=bytes.data()+off;if(e[0]==0)return false;if(e[0]==0xe5){lfn_parts.clear();lfn_active=false;continue;}
            if(e[11]==0x0f){const uint8_t ordinal=e[0]&0x1f;if(ordinal==0||ordinal>20){lfn_parts.clear();lfn_active=false;continue;}if(e[0]&0x40){lfn_parts.assign(ordinal,{});lfn_active=true;lfn_checksum=e[13];}if(!lfn_active||ordinal>lfn_parts.size()||e[13]!=lfn_checksum){lfn_parts.clear();lfn_active=false;continue;}
                auto& part=lfn_parts[ordinal-1];const size_t offsets[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};for(size_t i=0;i<13;++i)part[i]=U16(e+offsets[i]);continue;}
            if(e[11]&0x08){lfn_parts.clear();lfn_active=false;continue;}
            std::string decoded;
            if(lfn_active&&!lfn_parts.empty()){
                bool valid=Checksum(e)==lfn_checksum;
                for(const auto& part:lfn_parts)for(uint16_t code:part){if(code==0||code==0xffff)continue;if(code>0x7f){valid=false;break;}decoded.push_back(static_cast<char>(code));}
                if(!valid)decoded.clear();
            }
            if(Upper(decoded)==wanted||Upper(DecodeShort(e))==wanted){std::memcpy(found,e,32);if(entry_offset)*entry_offset=data_offset_+uint64_t(cluster-2)*cluster_size_+off;return true;}
            lfn_parts.clear();lfn_active=false;
        }
    }
    return false;
}

bool FatVolume::Insert(std::vector<uint32_t>* directory,const std::string& name,const uint8_t* entry,std::string* error){
    if(!writable_||name.empty()||name.size()>255){*error="FAT output name is invalid.";return false;}
    const size_t lfn_count=(name.size()+12)/13;std::array<uint8_t,11> short_name{};short_name.fill(' ');
    const auto dot=name.rfind('.');std::string base=dot==std::string::npos?name:name.substr(0,dot),ext=dot==std::string::npos?"":name.substr(dot+1);
    auto legal=[](char c){return std::isalnum(static_cast<unsigned char>(c))||c=='_'||c=='-'||c=='$'||c=='~';};
    std::string short_base,short_ext;
    for(char c:base)if(legal(c))short_base.push_back(c);
    for(char c:ext)if(legal(c)&&short_ext.size()<3)short_ext.push_back(c);
    short_base=Upper(short_base);short_ext=Upper(short_ext);
    if(short_base.empty())short_base="FILE";
    auto alias_used=[&](const std::array<uint8_t,11>& candidate,bool* ok){
        *ok=true;
        for(uint32_t cluster:*directory){
            std::vector<uint8_t> bytes;if(!ReadCluster(cluster,&bytes)){*ok=false;return true;}
            for(size_t off=0;off+32<=bytes.size();off+=32){
                const uint8_t* current=bytes.data()+off;
                if(current[0]==0)return false;
                if(current[0]==0xe5||current[11]==0x0f)continue;
                if(std::memcmp(current,candidate.data(),candidate.size())==0)return true;
            }
        }
        return false;
    };
    bool simple=base.size()<=8&&ext.size()<=3&&!base.empty();
    for(char c:base)if(!legal(c))simple=false;
    for(char c:ext)if(!legal(c))simple=false;
    if(simple){
        std::array<uint8_t,11> candidate{};candidate.fill(' ');
        const std::string exact_base=Upper(base),exact_ext=Upper(ext);
        std::memcpy(candidate.data(),exact_base.data(),exact_base.size());
        std::memcpy(candidate.data()+8,exact_ext.data(),exact_ext.size());
        bool ok=false;const bool used=alias_used(candidate,&ok);
        if(!ok){*error="Unable to inspect target FAT short-name aliases.";return false;}
        if(!used){short_name=candidate;short_base=exact_base;short_ext=exact_ext;}
        else simple=false;
    }
    if(!simple){
        // Pick a unique legal 8.3 alias. Several installed payload names share
        // their first six characters, so a fixed ~1 suffix creates duplicate
        // aliases that OVMF may reject when opening or renaming an LFN.
        bool found=false;
        for(uint32_t number=1;number<=999999&&!found;++number){
            const std::string suffix="~"+std::to_string(number);
            if(suffix.size()>=8)break;
            std::array<uint8_t,11> candidate{};candidate.fill(' ');
            std::string candidate_base=short_base.substr(0,8-suffix.size())+suffix;
            std::memcpy(candidate.data(),candidate_base.data(),candidate_base.size());
            std::memcpy(candidate.data()+8,short_ext.data(),short_ext.size());
            bool ok=false;const bool used=alias_used(candidate,&ok);
            if(!ok){*error="Unable to inspect target FAT short-name aliases.";return false;}
            if(!used){short_name=candidate;found=true;}
        }
        if(!found){*error="Unable to allocate a unique FAT short-name alias.";return false;}
    }
    std::vector<uint32_t> chain=*directory;size_t slot_count=cluster_size_/32;uint64_t slot=UINT64_MAX;
    for(size_t c=0;c<chain.size()&&slot==UINT64_MAX;++c){std::vector<uint8_t> bytes;if(!ReadCluster(chain[c],&bytes)){*error="Unable to inspect target FAT directory.";return false;}for(size_t i=0;i<slot_count;++i){if(bytes[i*32]==0||bytes[i*32]==0xe5){slot=c*slot_count+i;break;}}}
    const size_t slots_needed=lfn_count+1;
    if(slot==UINT64_MAX||slot%slot_count+slots_needed>slot_count){if(chain.empty()){*error="Target FAT directory has no root cluster.";return false;}uint32_t fresh;if(!Allocate(&fresh,error))return false;if(!SetFatEntry(chain.back(),fresh)){(void)SetFatEntry(fresh,0);*error="Unable to extend target FAT directory.";return false;}chain.push_back(fresh);*directory=chain;slot=(chain.size()-1)*slot_count;}
    const uint8_t sum=Checksum(short_name.data());
    for(size_t disk_index=0;disk_index<lfn_count;++disk_index){const size_t ordinal=lfn_count-disk_index;std::array<uint8_t,32> lfn{};lfn.fill(0xff);lfn[0]=static_cast<uint8_t>(ordinal|(ordinal==lfn_count?0x40:0));lfn[11]=0x0f;lfn[12]=0;lfn[13]=sum;P16(lfn.data()+26,0);
        const size_t offsets[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};const size_t begin=(ordinal-1)*13;
        for(size_t j=0;j<13;++j){uint16_t code=0xffff;if(begin+j<name.size())code=static_cast<uint8_t>(name[begin+j]);else if(begin+j==name.size())code=0;P16(lfn.data()+offsets[j],code);}
        const size_t cluster_i=static_cast<size_t>(slot+disk_index)/slot_count,entry_i=static_cast<size_t>(slot+disk_index)%slot_count;
        std::vector<uint8_t> bytes;if(!ReadCluster(chain[cluster_i],&bytes)){*error="Unable to read target FAT directory.";return false;}std::memcpy(bytes.data()+entry_i*32,lfn.data(),32);if(!WriteCluster(chain[cluster_i],bytes)){*error="Unable to store FAT long-name entry.";return false;}
    }
    const size_t short_slot=static_cast<size_t>(slot+lfn_count),cluster_i=short_slot/slot_count,entry_i=short_slot%slot_count;std::vector<uint8_t> bytes;if(!ReadCluster(chain[cluster_i],&bytes)){*error="Unable to read target FAT directory.";return false;}std::memcpy(bytes.data()+entry_i*32,short_name.data(),11);std::memcpy(bytes.data()+entry_i*32+11,entry+11,21);return WriteCluster(chain[cluster_i],bytes)||(*error="Unable to store FAT directory entry.",false);
}

bool FatVolume::ReadFile(const std::string& path,std::vector<uint8_t>* contents,std::string* error){
    auto components=Components(path);if(components.empty()){*error="FAT source path is empty.";return false;}std::vector<uint32_t> dir;std::string chain_error;if(!DirectoryChain(root_cluster_,&dir,&chain_error)){*error=chain_error;return false;}uint8_t entry[32]{};
    for(size_t i=0;i<components.size();++i){if(!Find(dir,components[i],entry,error)){*error="Approved live payload was not found on the live ESP: "+components[i];return false;}const uint8_t attr=entry[11];const uint32_t cluster=(uint32_t(U16(entry+20))<<16)|U16(entry+26);if(i+1<components.size()){if(!(attr&0x10)){*error="Live payload path component is not a directory.";return false;}if(!DirectoryChain(cluster,&dir,error))return false;}else{if(attr&0x10){*error="Live payload resolves to a directory.";return false;}const uint32_t size=U32(entry+28);if(size>512U*1024*1024){*error="Live payload exceeds the copy limit.";return false;}contents->resize(size);if(size==0)return true;std::vector<uint32_t> file_chain;if(!DirectoryChain(cluster,&file_chain,error))return false;size_t copied=0;for(uint32_t c:file_chain){std::vector<uint8_t> data;if(!ReadCluster(c,&data)){*error="Unable to read live FAT file data.";return false;}const size_t n=std::min(data.size(),contents->size()-copied);std::memcpy(contents->data()+copied,data.data(),n);copied+=n;if(copied==contents->size())return true;}*error="Live FAT file chain is shorter than its directory entry.";return false;}}
    return false;
}

bool FatVolume::WriteFile(const std::string& path,const std::vector<uint8_t>& contents,std::string* error){
    if(!writable_||contents.size()>512U*1024*1024){*error="Target FAT output is read-only or exceeds the copy limit.";return false;}
    const auto components=Components(path);if(components.empty()){*error="Target FAT path is empty.";return false;}std::vector<uint32_t> dir;std::string chain_error;if(!DirectoryChain(root_cluster_,&dir,&chain_error)){*error=chain_error;return false;}
    for(size_t i=0;i+1<components.size();++i){uint8_t entry[32]{};error->clear();if(Find(dir,components[i],entry,error)){if(!(entry[11]&0x10)){*error="Target path component exists as a file.";return false;}uint32_t c=(uint32_t(U16(entry+20))<<16)|U16(entry+26);if(!DirectoryChain(c,&dir,error))return false;}
        else{if(!error->empty())return false;uint32_t child;if(!Allocate(&child,error))return false;std::vector<uint8_t> dot_data(cluster_size_,0);dot_data[0]='.';std::fill(dot_data.begin()+1,dot_data.begin()+11,' ');dot_data[11]=0x10;P16(dot_data.data()+20,static_cast<uint16_t>(child>>16));P16(dot_data.data()+26,static_cast<uint16_t>(child));dot_data[32]='.';dot_data[33]='.';std::fill(dot_data.begin()+34,dot_data.begin()+43,' ');dot_data[43]=0x10;const uint32_t parent_cluster=dir.back()==root_cluster_?0:dir.back();P16(dot_data.data()+52,static_cast<uint16_t>(parent_cluster>>16));P16(dot_data.data()+58,static_cast<uint16_t>(parent_cluster));if(!WriteCluster(child,dot_data)){*error="Unable to initialize target FAT directory.";return false;}uint8_t new_entry[32]{};new_entry[11]=0x10;P16(new_entry+20,static_cast<uint16_t>(child>>16));P16(new_entry+26,static_cast<uint16_t>(child));if(!Insert(&dir,components[i],new_entry,error))return false;if(!DirectoryChain(child,&dir,error))return false;}}
    uint8_t existing_entry[32]{};uint64_t existing_offset=0;error->clear();const bool replacing=Find(dir,components.back(),existing_entry,error,&existing_offset);
    if(!replacing&&!error->empty())return false;
    if(replacing&&(existing_entry[11]&0x10)){*error="FAT output path already exists as a directory.";return false;}
    const uint32_t old_first=replacing?(uint32_t(U16(existing_entry+20))<<16)|U16(existing_entry+26):0;
    std::vector<uint32_t> old_chain;if(old_first&&!DirectoryChain(old_first,&old_chain,error))return false;
    uint32_t first_cluster=0;std::vector<uint32_t> allocated;
    auto release_new=[&](){for(uint32_t c:allocated)(void)SetFatEntry(c,0);};
    size_t remaining=contents.size();while(remaining){uint32_t c;if(!Allocate(&c,error)){release_new();return false;}allocated.push_back(c);remaining-=std::min<uint64_t>(remaining,cluster_size_);}
    for(size_t i=0;i<allocated.size();++i) {
        if(!SetFatEntry(allocated[i],i+1<allocated.size()?allocated[i+1]:kEoc)){*error="Unable to link target FAT file clusters.";release_new();return false;}
    }
    if(!allocated.empty())first_cluster=allocated.front();
    size_t offset=0;for(uint32_t c:allocated){std::vector<uint8_t> data(cluster_size_,0);const size_t n=std::min(data.size(),contents.size()-offset);std::memcpy(data.data(),contents.data()+offset,n);if(!WriteCluster(c,data)){*error="Unable to write target FAT file data.";release_new();return false;}offset+=n;}
    uint8_t entry[32]{};
    if(replacing)std::memcpy(entry,existing_entry,32);
    else entry[11]=0x20;
    entry[11]=0x20;P16(entry+20,static_cast<uint16_t>(first_cluster>>16));P16(entry+26,static_cast<uint16_t>(first_cluster));P32(entry+28,static_cast<uint32_t>(contents.size()));
    const bool wrote=replacing?WriteAt(fd_,entry,32,existing_offset):Insert(&dir,components.back(),entry,error);
    if(!wrote){if(replacing)*error="Unable to update existing FAT file entry.";release_new();return false;}
    for(uint32_t cluster:old_chain)if(!SetFatEntry(cluster,0)){*error="Unable to release the replaced FAT file data.";return false;}
    return true;
}

bool FatVolume::RenameFile(const std::string& from,const std::string& to,std::string* error) {
    if (!writable_) { *error="Target FAT volume is read-only."; return false; }
    if (from == to) return true;
    const auto a=Components(from), b=Components(to);
    if (a.empty() || b.empty() || a.size()!=b.size() || !std::equal(a.begin(),a.end()-1,b.begin())) {
        *error="FAT rename must stay in one directory."; return false;
    }
    const std::string& name=b.back(); const auto dot=name.find('.');
    const std::string base=dot==std::string::npos?name:name.substr(0,dot);
    const std::string ext=dot==std::string::npos?"":name.substr(dot+1);
    if (base.empty() || base.size()>8 || ext.size()>3 ||
        name.find('.',dot==std::string::npos?name.size():dot+1)!=std::string::npos) {
        *error="FAT rename destination must be a short 8.3 name."; return false;
    }
    std::vector<uint32_t> dir; if(!DirectoryChain(root_cluster_,&dir,error))return false;
    for(size_t i=0;i+1<a.size();++i){uint8_t e[32]{};if(!Find(dir,a[i],e,error))return false;uint32_t c=(uint32_t(U16(e+20))<<16)|U16(e+26);if(!DirectoryChain(c,&dir,error))return false;}
    uint8_t entry[32]{};uint64_t offset=0;if(!Find(dir,a.back(),entry,error,&offset)){*error="FAT rename source does not exist.";return false;}
    uint8_t collision[32]{};std::string collision_error;
    if(Find(dir,b.back(),collision,&collision_error)) {
        const uint32_t first=(uint32_t(U16(entry+20))<<16)|U16(entry+26), size=U32(entry+28);
        std::vector<uint8_t> contents(size); size_t copied=0;
        if(size){std::vector<uint32_t> chain;if(!DirectoryChain(first,&chain,error))return false;for(uint32_t c:chain){std::vector<uint8_t> data;if(!ReadCluster(c,&data))return false;size_t n=std::min(data.size(),contents.size()-copied);std::memcpy(contents.data()+copied,data.data(),n);copied+=n;if(copied==size)break;}if(copied!=size){*error="FAT rename source chain is truncated.";return false;}}
        if(!WriteFile(to,contents,error))return false;
        entry[0]=0xe5; return WriteAt(fd_,entry,32,offset);
    }
    if(!collision_error.empty()){*error=collision_error;return false;}
    std::memset(entry,' ',11);for(size_t i=0;i<base.size();++i)entry[i]=static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(base[i])));
    for(size_t i=0;i<ext.size();++i)entry[8+i]=static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(ext[i])));
    return WriteAt(fd_,entry,32,offset);
}

}  // namespace matonos::install
