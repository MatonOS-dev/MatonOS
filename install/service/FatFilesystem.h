#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace matonos::install {

class FatVolume {
  public:
    FatVolume() = default;
    ~FatVolume();
    FatVolume(const FatVolume&) = delete;
    FatVolume& operator=(const FatVolume&) = delete;
    FatVolume(FatVolume&& other) noexcept;
    FatVolume& operator=(FatVolume&& other) noexcept;
    static bool Open(const std::string& path,bool writable,FatVolume* out,std::string* error,
                     uint64_t expected_disk_sequence=0);
    bool ReadFile(const std::string& path,std::vector<uint8_t>* contents,std::string* error);
    bool WriteFile(const std::string& path,const std::vector<uint8_t>& contents,std::string* error);
    bool RenameFile(const std::string& from,const std::string& to,std::string* error);
  private:
    int fd_=-1;bool writable_=false;
    uint32_t bytes_per_sector_=0,sectors_per_cluster_=0,reserved_sectors_=0,fat_count_=0,sectors_per_fat_=0,root_cluster_=0,total_sectors_=0;
    uint64_t fat_offset_=0,data_offset_=0,cluster_size_=0,cluster_count_=0;
    uint32_t next_free_cluster_=2;
    bool Find(const std::vector<uint32_t>& directory,const std::string& name,uint8_t* entry,
              std::string* error,uint64_t* entry_offset=nullptr);
    bool DirectoryChain(uint32_t cluster,std::vector<uint32_t>* chain,std::string* error);
    bool ReadCluster(uint32_t cluster,std::vector<uint8_t>* bytes);
    bool WriteCluster(uint32_t cluster,const std::vector<uint8_t>& bytes);
    bool FatEntry(uint32_t cluster,uint32_t* value);
    bool SetFatEntry(uint32_t cluster,uint32_t value);
    bool Allocate(uint32_t* cluster,std::string* error);
    bool Insert(std::vector<uint32_t>* directory,const std::string& name,const uint8_t* entry,std::string* error);
};

}  // namespace matonos::install
