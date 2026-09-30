#include "LpMetadata.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace matonos::install {
namespace {
constexpr uint32_t kGeometryMagic=0x616c4467;
constexpr uint32_t kHeaderMagic=0x414c5030;
constexpr uint16_t kMajor=10, kMinor=2;
constexpr uint32_t kGeometryBytes=4096, kSectorBytes=512, kAlignmentBytes=1024*1024;
constexpr uint64_t kGeometryReservedBytes=4096;
constexpr uint64_t kGeometryCopiesBytes=kGeometryReservedBytes+8192;
constexpr uint32_t kReadonly=1;
constexpr uint32_t kLinear=0;
constexpr uint32_t kVirtualAbDevice=1;

#pragma pack(push,1)
struct Geometry { uint32_t magic,struct_size; uint8_t checksum[32]; uint32_t metadata_max_size,metadata_slot_count,logical_block_size; };
struct Table { uint32_t offset,num_entries,entry_size; };
struct Header {
    uint32_t magic; uint16_t major,minor; uint32_t header_size; uint8_t header_checksum[32];
    uint32_t tables_size; uint8_t tables_checksum[32];
    Table partitions,extents,groups,block_devices; uint32_t flags; uint8_t reserved[124];
};
struct LpPartition { char name[36]; uint32_t attributes,first_extent_index,num_extents,group_index; };
struct LpExtent { uint64_t num_sectors; uint32_t target_type; uint64_t target_data; uint32_t target_source; };
struct LpGroup { char name[36]; uint32_t flags; uint64_t maximum_size; };
struct BlockDevice { uint64_t first_logical_sector; uint32_t alignment,alignment_offset; uint64_t size; char partition_name[36]; uint32_t flags; };
#pragma pack(pop)
static_assert(sizeof(Geometry)==52 && sizeof(Header)==256 && sizeof(LpPartition)==52 && sizeof(LpExtent)==24 && sizeof(LpGroup)==48 && sizeof(BlockDevice)==64);

constexpr uint32_t kShaK[64]={
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
uint32_t R(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
std::array<uint8_t,32> Sha256(const uint8_t* data,size_t size) {
    std::array<uint32_t,8> h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint64_t bit_size=static_cast<uint64_t>(size)*8;
    const size_t total=((size+9+63)/64)*64;
    std::array<uint8_t,64> block{};
    for(size_t base=0;base<total;base+=64) {
        block.fill(0);
        for(size_t i=0;i<64&&base+i<size;++i)block[i]=data[base+i];
        if(base<=size&&size<base+64)block[size-base]=0x80;
        if(base+64==total)for(int i=0;i<8;++i)block[63-i]=static_cast<uint8_t>(bit_size>>(8*i));
        uint32_t w[64];
        for(int i=0;i<16;++i)w[i]=(uint32_t(block[i*4])<<24)|(uint32_t(block[i*4+1])<<16)|(uint32_t(block[i*4+2])<<8)|block[i*4+3];
        for(int i=16;i<64;++i){uint32_t s0=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3);uint32_t s1=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for(int i=0;i<64;++i){uint32_t s1=R(e,6)^R(e,11)^R(e,25),ch=(e&f)^(~e&g);uint32_t t1=z+s1+ch+kShaK[i]+w[i];uint32_t s0=R(a,2)^R(a,13)^R(a,22),maj=(a&b)^(a&c)^(b&c);uint32_t t2=s0+maj;z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    std::array<uint8_t,32> out{};
    for(int i=0;i<8;++i)for(int j=0;j<4;++j)out[i*4+j]=static_cast<uint8_t>(h[i]>>(24-8*j));
    return out;
}
template<class T> void Append(std::vector<uint8_t>* bytes,const T& value) { const auto* p=reinterpret_cast<const uint8_t*>(&value);bytes->insert(bytes->end(),p,p+sizeof(T)); }
bool WriteAt(int fd,const uint8_t* data,size_t size,uint64_t offset) {
    size_t done=0;while(done<size){ssize_t n=pwrite(fd,data+done,size-done,static_cast<off_t>(offset+done));if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=static_cast<size_t>(n);}return true;
}
bool ReadAt(int fd,uint8_t* data,size_t size,uint64_t offset) {
    size_t done=0;while(done<size){ssize_t n=pread(fd,data+done,size-done,static_cast<off_t>(offset+done));if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=static_cast<size_t>(n);}return true;
}
bool ValidName(const std::string& name) {
    if(name.empty()||name.size()>36)return false;
    for(char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))return false;
    return true;
}
template<size_t N> void SetName(char (&dest)[N],const std::string& source){std::memset(dest,0,N);std::memcpy(dest,source.data(),std::min(source.size(),N));}
bool MakeMetadata(const CreateLpMetadata& request,uint64_t super_size,const std::string& super_name,std::vector<uint8_t>* output,uint64_t* first_sector,std::string* error) {
    if(request.metadata_size_bytes<512||request.metadata_size_bytes>1024*1024||request.metadata_size_bytes%512||request.metadata_size_bytes>UINT32_MAX||request.metadata_slots<2||request.metadata_slots>3){*error="LP metadata geometry is outside the supported v1 range.";return false;}
    std::vector<LpGroup> groups(1);SetName(groups[0].name,"default");
    std::vector<LpPartition> partitions;
    std::vector<LpExtent> extents;
    std::vector<std::string> group_names{"default"},partition_names;
    uint64_t cursor_bytes=kGeometryCopiesBytes+uint64_t(request.metadata_size_bytes)*request.metadata_slots*2;
    cursor_bytes=(cursor_bytes+kAlignmentBytes-1)/kAlignmentBytes*kAlignmentBytes;
    *first_sector=cursor_bytes/kSectorBytes;
    for(const auto& group:request.groups) {
        if(!ValidName(group.name)||group.name=="default"||std::find(group_names.begin(),group_names.end(),group.name)!=group_names.end()) {*error="Invalid or duplicate LP group name.";return false;}
        group_names.push_back(group.name);LpGroup g{};SetName(g.name,group.name);g.maximum_size=group.maximum_size_bytes;groups.push_back(g);
        const uint32_t group_index=static_cast<uint32_t>(groups.size()-1);
        uint64_t member_sum=0;
        for(const auto& member:group.partitions) {
            if(!ValidName(member.name)||std::find(partition_names.begin(),partition_names.end(),member.name)!=partition_names.end()||(member.size_bytes!=0&&member.size_bytes%kAlignmentBytes)){*error="Invalid or duplicate LP partition name or size.";return false;}
            if(member_sum>UINT64_MAX-member.size_bytes){*error="LP partition sizes overflow.";return false;}member_sum+=member.size_bytes;
            LpPartition p{};SetName(p.name,member.name);p.attributes=kReadonly;p.first_extent_index=static_cast<uint32_t>(extents.size());p.num_extents=member.size_bytes==0?0:1;p.group_index=group_index;partitions.push_back(p);
            if(member.size_bytes!=0){
                const uint64_t extent_sectors=member.size_bytes/kSectorBytes;
                if(cursor_bytes>super_size||member.size_bytes>super_size-cursor_bytes){*error="LP partitions exceed the super extent.";return false;}
                LpExtent extent{};extent.num_sectors=extent_sectors;extent.target_type=kLinear;extent.target_data=cursor_bytes/kSectorBytes;extent.target_source=0;extents.push_back(extent);
            }
            partition_names.push_back(member.name);cursor_bytes+=member.size_bytes;
        }
        if(member_sum>group.maximum_size_bytes){*error="LP group members exceed their declared capacity.";return false;}
    }
    if(partitions.empty()){*error="LP metadata has no partitions.";return false;}
    BlockDevice device{};device.first_logical_sector=*first_sector;device.alignment=kAlignmentBytes;device.alignment_offset=0;device.size=super_size;SetName(device.partition_name,super_name);
    std::vector<uint8_t> tables;const uint32_t part_offset=0;for(const auto& p:partitions)Append(&tables,p);
    const uint32_t extent_offset=static_cast<uint32_t>(tables.size());for(const auto& e:extents)Append(&tables,e);
    const uint32_t group_offset=static_cast<uint32_t>(tables.size());for(const auto& g:groups)Append(&tables,g);
    const uint32_t device_offset=static_cast<uint32_t>(tables.size());Append(&tables,device);
    if(sizeof(Header)+tables.size()>request.metadata_size_bytes){*error="LP tables exceed the metadata allocation.";return false;}
    Header header{};header.magic=kHeaderMagic;header.major=kMajor;header.minor=kMinor;header.header_size=sizeof(Header);header.tables_size=static_cast<uint32_t>(tables.size());header.flags=kVirtualAbDevice;
    header.partitions={part_offset,static_cast<uint32_t>(partitions.size()),sizeof(LpPartition)};
    header.extents={extent_offset,static_cast<uint32_t>(extents.size()),sizeof(LpExtent)};
    header.groups={group_offset,static_cast<uint32_t>(groups.size()),sizeof(LpGroup)};
    header.block_devices={device_offset,1,sizeof(BlockDevice)};
    auto checksum=Sha256(tables.data(),tables.size());std::memcpy(header.tables_checksum,checksum.data(),checksum.size());
    std::array<uint8_t,sizeof(Header)> header_copy{};std::memcpy(header_copy.data(),&header,sizeof(header));
    checksum=Sha256(header_copy.data(),header_copy.size());std::memcpy(header.header_checksum,checksum.data(),checksum.size());
    output->resize(sizeof(Header)+tables.size());std::memcpy(output->data(),&header,sizeof(header));std::memcpy(output->data()+sizeof(Header),tables.data(),tables.size());
    return true;
}
bool ParseMetadata(int fd,const std::string& super_guid,const std::string& super_name,uint64_t super_size,std::vector<Partition>* partitions) {
    Geometry geometry{};if(!ReadAt(fd,reinterpret_cast<uint8_t*>(&geometry),sizeof(geometry),kGeometryReservedBytes)||geometry.magic!=kGeometryMagic||geometry.struct_size!=sizeof(Geometry)||geometry.metadata_slot_count<2||geometry.metadata_max_size<512||geometry.metadata_max_size>1024*1024)return false;
    auto geometry_copy=geometry;std::memset(geometry_copy.checksum,0,sizeof(geometry_copy.checksum));auto hash=Sha256(reinterpret_cast<uint8_t*>(&geometry_copy),sizeof(geometry_copy));if(std::memcmp(hash.data(),geometry.checksum,32)!=0)return false;
    const uint64_t metadata_offset=kGeometryCopiesBytes;
    std::vector<uint8_t> meta(geometry.metadata_max_size);
    if(!ReadAt(fd,meta.data(),meta.size(),metadata_offset))return false;
    if(meta.size()<sizeof(Header))return false;
    Header header{};std::memcpy(&header,meta.data(),sizeof(header));
    if(header.magic!=kHeaderMagic||header.major!=kMajor||header.header_size!=sizeof(Header)||header.tables_size>meta.size()-sizeof(Header))return false;
    auto header_copy=header;std::memset(header_copy.header_checksum,0,sizeof(header_copy.header_checksum));hash=Sha256(reinterpret_cast<uint8_t*>(&header_copy),header.header_size);if(std::memcmp(hash.data(),header.header_checksum,32)!=0)return false;
    const uint8_t* table_bytes=meta.data()+sizeof(Header);hash=Sha256(table_bytes,header.tables_size);if(std::memcmp(hash.data(),header.tables_checksum,32)!=0)return false;
    auto table_valid=[&](const Table& t,uint32_t entry_size){return t.entry_size==entry_size&&uint64_t(t.offset)+uint64_t(t.num_entries)*entry_size<=header.tables_size;};
    if(!table_valid(header.partitions,sizeof(LpPartition))||!table_valid(header.extents,sizeof(LpExtent))||!table_valid(header.block_devices,sizeof(BlockDevice))||header.block_devices.num_entries!=1)return false;
    BlockDevice device{};std::memcpy(&device,table_bytes+header.block_devices.offset,sizeof(device));
    if(device.size!=super_size||std::string(device.partition_name,strnlen(device.partition_name,sizeof(device.partition_name)))!=super_name)return false;
    for(uint32_t i=0;i<header.partitions.num_entries;++i) {
        LpPartition p{};std::memcpy(&p,table_bytes+header.partitions.offset+uint64_t(i)*sizeof(p),sizeof(p));
        if(p.num_extents==0){if(p.first_extent_index>header.extents.num_entries) return false;continue;}
        if(p.num_extents!=1||p.first_extent_index>=header.extents.num_entries)return false;
        LpExtent extent{};std::memcpy(&extent,table_bytes+header.extents.offset+uint64_t(p.first_extent_index)*sizeof(extent),sizeof(extent));
        if(extent.target_type!=kLinear||extent.target_source!=0||extent.num_sectors>UINT64_MAX/kSectorBytes||extent.target_data>UINT64_MAX/kSectorBytes)return false;
        const uint64_t start=extent.target_data*kSectorBytes,size=extent.num_sectors*kSectorBytes;
        if(start>super_size||size>super_size-start||start<device.first_logical_sector*kSectorBytes)return false;
        Partition logical{};logical.name=std::string(p.name,strnlen(p.name,sizeof(p.name)));logical.size_bytes=size;logical.start_bytes=start;logical.logical=true;logical.backing_part_guid=super_guid;partitions->push_back(std::move(logical));
    }
    return true;
}
}  // namespace

bool WriteLpMetadata(const std::string& super_path,uint64_t super_size_bytes,const std::string& super_partition_name,const CreateLpMetadata& request,uint64_t disk_sequence,std::string* error) {
    int fd=open(super_path.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(fd<0){*error="Cannot open super partition for LP metadata.";return false;}
    uint64_t opened_sequence=0;if(!disk_sequence||ioctl(fd,BLKGETDISKSEQ,&opened_sequence)!=0||opened_sequence!=disk_sequence){close(fd);*error="Selected disk changed before opening its super partition.";return false;}
    uint64_t actual=0;
    if(ioctl(fd,BLKGETSIZE64,&actual)!=0) {
#ifdef MATONOS_LP_METADATA_TEST
        struct stat st{};if(fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>=0)actual=static_cast<uint64_t>(st.st_size);
#endif
    }
    if(actual!=super_size_bytes){close(fd);*error="Super block-device size changed before LP initialization.";return false;}
    std::vector<uint8_t> metadata;uint64_t first_sector=0;
    if(!MakeMetadata(request,super_size_bytes,super_partition_name,&metadata,&first_sector,error)){close(fd);return false;}
    Geometry geometry{};geometry.magic=kGeometryMagic;geometry.struct_size=sizeof(Geometry);geometry.metadata_max_size=static_cast<uint32_t>(request.metadata_size_bytes);geometry.metadata_slot_count=request.metadata_slots;geometry.logical_block_size=4096;
    auto checksum=Sha256(reinterpret_cast<uint8_t*>(&geometry),sizeof(geometry));std::memcpy(geometry.checksum,checksum.data(),checksum.size());
    std::array<uint8_t,kGeometryBytes> block{};std::memcpy(block.data(),&geometry,sizeof(geometry));
    bool ok=WriteAt(fd,block.data(),block.size(),kGeometryReservedBytes)&&WriteAt(fd,block.data(),block.size(),kGeometryReservedBytes+kGeometryBytes);
    std::vector<uint8_t> padded(request.metadata_size_bytes,0);std::memcpy(padded.data(),metadata.data(),metadata.size());
    const uint64_t primary=kGeometryCopiesBytes;
    const uint64_t backup=primary+uint64_t(request.metadata_size_bytes)*request.metadata_slots;
    for(uint32_t slot=0;ok&&slot<request.metadata_slots;++slot){ok=WriteAt(fd,padded.data(),padded.size(),primary+uint64_t(slot)*request.metadata_size_bytes)&&WriteAt(fd,padded.data(),padded.size(),backup+uint64_t(slot)*request.metadata_size_bytes);}
    if(ok&&fsync(fd)!=0)ok=false;
    if(ok){std::vector<uint8_t> verify(padded.size());for(uint32_t slot=0;slot<request.metadata_slots;++slot){if(!ReadAt(fd,verify.data(),verify.size(),primary+uint64_t(slot)*request.metadata_size_bytes)||verify!=padded){ok=false;break;}}}
    close(fd);if(!ok)*error="LP metadata write or readback verification failed.";return ok;
}

bool ReadLpLogicalPartitions(const std::string& super_path,const std::string& super_part_guid,const std::string& super_partition_name,uint64_t super_size_bytes,std::vector<Partition>* partitions) {
    int fd=open(super_path.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)return false;
    const bool ok=ParseMetadata(fd,super_part_guid,super_partition_name,super_size_bytes,partitions);close(fd);return ok;
}
}  // namespace matonos::install
