// SPDX-License-Identifier: Apache-2.0
// Privileged, fail-closed installer for user supplied out-of-tree modules.
#include <android/log.h>
#include <matonos_ipc.h>
#include <sys/system_properties.h>
#include "package_zip.h"
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <signal.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <set>
#include <utility>
#include <zlib.h>

namespace fs = std::filesystem;
namespace {
constexpr char kSlot[] = "/mnt/vendor/addons";
constexpr char kData[] = "/data/vendor/matonos-addons";
constexpr uint64_t kSlotImageBytes = 512ULL * 1024ULL * 1024ULL;
constexpr char kRepoPublicKey[] = "/odm/etc/addons/repo.pub";
constexpr size_t kMaxPackage = 128U * 1024U * 1024U;
constexpr size_t kChunkDecoded = 32U * 1024U;
constexpr char kLog[] = "matonos-addons";
MatonosIpcServer* gIpc;
std::mutex gLock;
struct Upload { bool active=false, image=false; int fd=-1; std::string operation_id, path; uint64_t size=0, received=0; } gUpload;
struct PendingImage { bool valid=false; std::string id,version,sha256,manifest; std::vector<uint8_t> signature; uint64_t size=0; } gPending;

std::string Trim(std::string s) { while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back(); return s; }
bool SafeText(const std::string& s, size_t max=160) {
    if (s.empty() || s.size()>max) return false;
    for (unsigned char c:s) if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c==' '||c=='_'||c=='-'||c=='.'||c==':'||c=='/'||c=='+'||c=='@'||c=='('||c==')')) return false;
    return true;
}
bool Name(const std::string& s) {
    if (s.empty() || s.size()>64) return false;
    return std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';});
}
bool IsSha256(const std::string& s) {
    return s.size()==64&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
std::string JsonEscape(const std::string& s) { std::string o="\""; for(char c:s){ if(c=='"'||c=='\\')o+='\\'; if(c=='\n'||c=='\r')continue; o+=c; } return o+'"'; }

// The channel helper has already parsed the outer JSON. This strict extractor
// accepts only primitive string/unsigned integer members used by this API.
bool JsonField(const char* json,const char* key,std::string* out) {
    const std::string needle=std::string("\"")+key+"\""; const char* p=strstr(json,needle.c_str());
    if(!p)return false; p+=needle.size(); while(*p==' '||*p=='\t')++p; if(*p++!=':')return false;
    while(*p==' '||*p=='\t')++p; if(*p++!='\"')return false;
    out->clear(); while(*p&&*p!='\"'){ if(*p=='\\'||(unsigned char)*p<0x20)return false; out->push_back(*p++); }
    return *p=='\"';
}
bool JsonNumber(const char* json,const char* key,uint64_t* out) {
    const std::string needle=std::string("\"")+key+"\""; const char* p=strstr(json,needle.c_str()); if(!p)return false;
    p+=needle.size(); while(*p==' '||*p=='\t')++p; if(*p++!=':')return false; while(*p==' '||*p=='\t')++p;
    if(*p<'0'||*p>'9')return false; uint64_t v=0; do { unsigned d=(unsigned)(*p++-'0'); if(v>(UINT64_MAX-d)/10)return false; v=v*10+d; } while(*p>='0'&&*p<='9'); *out=v; return true;
}
bool SafeVersion(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+';
    });
}
bool SafeComponent(const std::string& s) {
    if (s.empty() || s.size() > 128 || s == "." || s == "..") return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+';
    });
}
bool JsonObjectArray(const std::string& json, const char* key, std::vector<std::string>* objects) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p++] != '[') return false;
    while (p < json.size()) {
        p = json.find_first_not_of(" \t\r\n,", p);
        if (p == std::string::npos) return false;
        if (json[p] == ']') return true;
        if (json[p] != '{') return false;
        const size_t start = p;
        int depth = 0; bool quoted = false, escaped = false;
        for (; p < json.size(); ++p) {
            const char c = json[p];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { ++p; break; }
        }
        if (depth != 0 || quoted) return false;
        objects->push_back(json.substr(start, p - start));
    }
    return false;
}
bool JsonStringArray(const std::string& json,const char* key,std::vector<std::string>* values){
    const std::string needle=std::string("\"")+key+"\"";size_t p=json.find(needle);if(p==std::string::npos)return false;
    p=json.find(':',p+needle.size());if(p==std::string::npos)return false;p=json.find_first_not_of(" \t\r\n",p+1);
    if(p==std::string::npos||json[p++]!='[')return false;
    while(p<json.size()){
        p=json.find_first_not_of(" \t\r\n,",p);if(p==std::string::npos)return false;if(json[p]==']')return true;if(json[p++]!='\"')return false;
        const size_t start=p;while(p<json.size()&&json[p]!='\"'){if(json[p]=='\\'||static_cast<unsigned char>(json[p])<0x20)return false;++p;}
        if(p==json.size())return false;values->push_back(json.substr(start,p-start));++p;
    }
    return false;
}

// Small SHA-256 implementation keeps this NDK daemon independent of private
// platform libraries. Input hashes cover the complete file including signature.
struct Sha256 {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t bits=0; uint8_t block[64]{}; size_t used=0;
    static uint32_t R(uint32_t x,int n){return (x>>n)|(x<<(32-n));}
    void Transform(const uint8_t* b){
        static const uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64]; for(int i=0;i<16;i++)w[i]=(uint32_t)b[i*4]<<24|(uint32_t)b[i*4+1]<<16|(uint32_t)b[i*4+2]<<8|b[i*4+3];
        for(int i=16;i<64;i++){uint32_t a=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3),c=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+a+w[i-7]+c;}
        uint32_t a=h[0],b0=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for(int i=0;i<64;i++){uint32_t t1=z+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+k[i]+w[i],t2=(R(a,2)^R(a,13)^R(a,22))+((a&b0)^(a&c)^(b0&c));z=g;g=f;f=e;e=d+t1;d=c;c=b0;b0=a;a=t1+t2;}
        h[0]+=a;h[1]+=b0;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    void Add(const uint8_t* p,size_t n){bits+=(uint64_t)n*8;while(n){size_t take=std::min(n,64-used);memcpy(block+used,p,take);used+=take;p+=take;n-=take;if(used==64){Transform(block);used=0;}}}
    std::string Finish(){uint64_t original=bits;uint8_t one=0x80;Add(&one,1);uint8_t zero=0;while(used!=56)Add(&zero,1);uint8_t len[8];for(int i=0;i<8;i++)len[7-i]=(uint8_t)(original>>(i*8));Add(len,8);static const char x[]="0123456789abcdef";std::string out;for(uint32_t v:h)for(int i=7;i>=0;i--)out+=x[(v>>(i*4))&15];return out;}
};
std::string Hash(const std::vector<uint8_t>& b){Sha256 s;s.Add(b.data(),b.size());return s.Finish();}
bool WriteAll(int fd,const void* data,size_t n);
bool HashFile(const fs::path& path,uint64_t expected,std::string* digest){
    std::ifstream f(path,std::ios::binary);if(!f)return false;Sha256 hash;uint8_t buf[65536];uint64_t total=0;
    while(f){f.read(reinterpret_cast<char*>(buf),sizeof(buf));const auto n=f.gcount();if(n>0){total+=(uint64_t)n;if(total>expected)return false;hash.Add(buf,(size_t)n);}}
    if(!f.eof()||total!=expected)return false;*digest=hash.Finish();return true;
}
bool ExpandImageGzip(const fs::path& compressed,const fs::path& raw){
    int in=open(compressed.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW),out=open(raw.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(in<0||out<0){if(in>=0)close(in);if(out>=0)close(out);return false;}
    z_stream z{};bool ok=inflateInit2(&z,15+32)==Z_OK;uint8_t ib[65536],ob[65536];uint64_t total=0;bool ended=false;
    while(ok&&!ended){ssize_t n=read(in,ib,sizeof(ib));if(n<0&&errno==EINTR)continue;if(n<0){ok=false;break;}if(n==0){ok=false;break;}
        z.next_in=ib;z.avail_in=(uInt)n;
        while(ok&&z.avail_in){z.next_out=ob;z.avail_out=sizeof(ob);int rc=inflate(&z,Z_NO_FLUSH);size_t made=sizeof(ob)-z.avail_out;
            if(total+made>kSlotImageBytes||!WriteAll(out,ob,made)){ok=false;break;}total+=made;
            if(rc==Z_STREAM_END){ended=true;if(z.avail_in!=0)ok=false;break;}if(rc!=Z_OK){ok=false;break;}
        }
        if(ended){uint8_t extra;if(read(in,&extra,1)!=0)ok=false;break;}
    }
    if(ok)ok=ended&&total==kSlotImageBytes&&fsync(out)==0;
    inflateEnd(&z);close(in);close(out);if(!ok)unlink(raw.c_str());return ok;
}

struct Metadata { std::string name,vermagic,license,alias,signer; };
bool Range(size_t total,uint64_t off,uint64_t len){return off<=total&&len<=total-off;}
bool ReadModinfo(const std::vector<uint8_t>& b, Metadata* m, std::string* err){
    if(b.size()<sizeof(Elf64_Ehdr)){*err="module is too small to be ELF64";return false;}
    const auto* e=reinterpret_cast<const Elf64_Ehdr*>(b.data());
    if(memcmp(e->e_ident,ELFMAG,SELFMAG)||e->e_ident[EI_CLASS]!=ELFCLASS64||e->e_ident[EI_DATA]!=ELFDATA2LSB||e->e_machine!=EM_X86_64||e->e_type!=ET_REL){*err="expected little-endian x86_64 relocatable ELF module";return false;}
    if(e->e_shentsize!=sizeof(Elf64_Shdr)||!e->e_shnum||e->e_shnum>4096||!Range(b.size(),e->e_shoff,(uint64_t)e->e_shnum*sizeof(Elf64_Shdr))||e->e_shstrndx>=e->e_shnum){*err="invalid ELF section table";return false;}
    const auto* sh=reinterpret_cast<const Elf64_Shdr*>(b.data()+e->e_shoff);const auto& strings=sh[e->e_shstrndx];
    if(!Range(b.size(),strings.sh_offset,strings.sh_size)){*err="invalid ELF section names";return false;}
    const char* names=(const char*)b.data()+strings.sh_offset;const Elf64_Shdr* mod=nullptr;
    for(unsigned i=0;i<e->e_shnum;i++)if(sh[i].sh_name<strings.sh_size){
        const size_t left=(size_t)strings.sh_size-sh[i].sh_name;
        const char* end=(const char*)memchr(names+sh[i].sh_name,0,left);
        if(end&&std::string_view(names+sh[i].sh_name,(size_t)(end-(names+sh[i].sh_name)))==".modinfo"){mod=&sh[i];break;}
    }
    if(!mod||!Range(b.size(),mod->sh_offset,mod->sh_size)){*err="ELF .modinfo section is missing or invalid";return false;}
    const char* p=(const char*)b.data()+mod->sh_offset;size_t left=(size_t)mod->sh_size;
    while(left){size_t n=strnlen(p,left);if(n==left)break;std::string row(p,n);auto eq=row.find('=');if(eq!=std::string::npos){std::string key=row.substr(0,eq),v=row.substr(eq+1);if(key=="name")m->name=v;else if(key=="vermagic")m->vermagic=v;else if(key=="license")m->license=v;else if(key=="alias"){if(!m->alias.empty())m->alias+=';';m->alias+=v;}else if(key=="signer")m->signer=v;}p+=n+1;left-=n+1;}
    m->name=Trim(m->name);m->vermagic=Trim(m->vermagic);m->license=Trim(m->license);m->alias=Trim(m->alias);
    if(!Name(m->name)||m->vermagic.empty()||m->license.empty()||m->alias.empty()){*err="module requires valid name, vermagic, license and modalias metadata";return false;}
    return true;
}
bool ReadFileLimit(const fs::path& p,size_t limit,std::vector<uint8_t>* b){std::ifstream f(p,std::ios::binary);if(!f)return false;f.seekg(0,std::ios::end);auto n=f.tellg();if(n<0||(uint64_t)n>limit)return false;b->resize((size_t)n);f.seekg(0);return b->empty()||!!f.read((char*)b->data(),(std::streamsize)b->size());}
bool WriteAll(int fd,const void* data,size_t n){const char* p=(const char*)data;while(n){ssize_t r=write(fd,p,n);if(r<0){if(errno==EINTR)continue;return false;}p+=r;n-=(size_t)r;}return true;}
bool WritePackageFile(const fs::path& path,const uint8_t* data,size_t size,mode_t mode){int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,mode);if(fd<0)return false;bool ok=WriteAll(fd,data,size)&&fsync(fd)==0;close(fd);return ok;}
[[maybe_unused]] bool EnsureSafeDirectories(const fs::path& root,const fs::path& relative){
    fs::path current=root;
    for(const auto& component:relative){
        const std::string name=component.string();if(name.empty()||name=="."||name=="..")return false;
        current/=component;struct stat st{};
        if(lstat(current.c_str(),&st)!=0){if(errno!=ENOENT||mkdir(current.c_str(),0755)!=0)return false;if(lstat(current.c_str(),&st)!=0)return false;}
        if(!S_ISDIR(st.st_mode))return false;
    }
    return true;
}
std::string KernelRelease(){std::ifstream f("/proc/sys/kernel/osrelease");std::string s;std::getline(f,s);return Trim(s);}
bool IsSlotMounted(){
    std::ifstream f("/proc/self/mountinfo");std::string row;const std::string needle=std::string(" ")+kSlot+" ";
    while(std::getline(f,row))if(row.find(needle)!=std::string::npos)return true;
    return false;
}
struct PackageModules { std::string id, version; std::vector<std::string> modules, firmware; };
bool Loaded(const std::string& name);
bool RunModprobe(const std::string& addon,const std::string& version,const std::string& name);
bool ValidatePackage(matonos_addons::VerifiedZip* package,PackageModules* parsed,std::string* err){
    const std::string& manifest=package->manifest;std::string id,version,release,source,license,image_hash;uint64_t format=0,image_size=0;
    const bool has_image_hash=JsonField(manifest.c_str(),"imageSha256",&image_hash);
    const bool has_image_size=JsonNumber(manifest.c_str(),"imageSize",&image_size);
    if(!JsonNumber(manifest.c_str(),"format",&format)||format!=1||!JsonField(manifest.c_str(),"id",&id)||!Name(id)||id=="firmware"||
       !JsonField(manifest.c_str(),"version",&version)||!SafeVersion(version)||!JsonField(manifest.c_str(),"kernelRelease",&release)||release!=KernelRelease()||
       !JsonField(manifest.c_str(),"source",&source)||!SafeText(source)||!JsonField(manifest.c_str(),"license",&license)||!SafeText(license)||
       (has_image_hash!=has_image_size)||(has_image_hash&&(!IsSha256(image_hash)||image_size!=kSlotImageBytes))){
        *err="signed manifest identity, provenance, format, or kernel release is invalid";return false;
    }
    std::set<std::string> declared;
    for(const char* array:{"modules","firmware","daemons"}){
        std::vector<std::string> records;if(!JsonObjectArray(manifest,array,&records)){*err=std::string("invalid ")+array+" manifest array";return false;}
        if(strcmp(array,"daemons")==0&&!records.empty()){*err="package daemons are not enabled in this immutable-slot release";return false;}
        for(const auto& record:records){
            std::string path,hash;uint64_t size=0;
            bool safe_path=JsonField(record.c_str(),"path",&path);size_t path_pos=0;
            while(safe_path&&path_pos<path.size()){size_t slash=path.find('/',path_pos);if(slash==std::string::npos)slash=path.size();if(!SafeComponent(path.substr(path_pos,slash-path_pos)))safe_path=false;path_pos=slash+1;}
            if(!safe_path||path.size()>255||!JsonField(record.c_str(),"sha256",&hash)||hash.size()!=64||
               !JsonNumber(record.c_str(),"size",&size)||!declared.insert(path).second){*err="invalid or duplicate manifest member";return false;}
            const bool prefix=(strcmp(array,"modules")==0&&path.rfind("modules/",0)==0)||(strcmp(array,"firmware")==0&&path.rfind("firmware/",0)==0)||(strcmp(array,"daemons")==0&&path.rfind("bin/",0)==0);
            auto item=package->files.find(path);
            if(!prefix||item==package->files.end()||item->second.size()!=size||Hash(item->second)!=hash){*err="package member is missing or differs from signed size/hash";return false;}
            if(strcmp(array,"modules")==0){
                std::string name,vermagic,module_license,module_source,source_license;std::vector<std::string> aliases;Metadata m;const std::string filename=path.substr(8);
                if(filename.size()<=3||filename.substr(filename.size()-3)!=".ko"||!JsonField(record.c_str(),"name",&name)||!Name(name)||filename!=name+".ko"||
                   !JsonField(record.c_str(),"vermagic",&vermagic)||!JsonField(record.c_str(),"license",&module_license)||!SafeText(module_license)||
                   !JsonField(record.c_str(),"source",&module_source)||!SafeText(module_source)||!JsonField(record.c_str(),"sourceLicense",&source_license)||!SafeText(source_license)||
                   !JsonStringArray(record,"aliases",&aliases)||aliases.empty()||
                   !ReadModinfo(item->second,&m,err)||m.name!=name||m.vermagic!=vermagic||m.license!=module_license||
                   m.vermagic.compare(0,release.size(),release)!=0||(m.vermagic.size()>release.size()&&m.vermagic[release.size()]!=' ')){
                    *err="module ELF metadata does not match signed manifest or running kernel";return false;
                }
                std::vector<std::string> actual;std::stringstream alias_stream(m.alias);std::string alias;
                while(std::getline(alias_stream,alias,';'))if(!alias.empty())actual.push_back(alias);
                std::sort(actual.begin(),actual.end());std::sort(aliases.begin(),aliases.end());
                if(actual!=aliases){*err="module modalias metadata does not match signed manifest";return false;}
                parsed->modules.push_back(name);
            }
            if(strcmp(array,"firmware")==0){
                const std::string name=path.substr(9);
                size_t pos=0;
                while(pos<name.size()){
                    size_t slash=name.find('/',pos);if(slash==std::string::npos)slash=name.size();
                    if(!SafeComponent(name.substr(pos,slash-pos))){*err="firmware path contains an unsafe component";return false;}
                    pos=slash+1;
                }
                parsed->firmware.push_back(name);
            }
        }
    }
    if(declared.size()!=package->files.size()){*err="ZIP has an undeclared payload member";return false;}
    parsed->id=id;parsed->version=version;return true;
}
bool LoadInstalledPackage(const fs::path& base,const std::string& expected_id,const std::string& expected_version,
                          matonos_addons::VerifiedZip* package,PackageModules* parsed,std::string* err){
    std::vector<uint8_t> manifest,signature;
    if(!ReadFileLimit(base/"manifest.json",1024U*1024U,&manifest)||!ReadFileLimit(base/"manifest.sig",64,&signature)||signature.size()!=64){
        *err="installed signed manifest or signature is missing/oversized";return false;
    }
    package->manifest.assign(manifest.begin(),manifest.end());package->signature=std::move(signature);
    if(!matonos_addons::VerifyManifestSignature(package->manifest,package->signature,kRepoPublicKey,err))return false;
    for(const char* array:{"modules","firmware","daemons"}){
        std::vector<std::string> records;if(!JsonObjectArray(package->manifest,array,&records)){*err="installed manifest has an invalid member array";return false;}
        for(const auto& record:records){
            std::string path;if(!JsonField(record.c_str(),"path",&path)){*err="installed member path is missing";return false;}
            size_t p=0;while(p<path.size()){size_t slash=path.find('/',p);if(slash==std::string::npos)slash=path.size();if(!SafeComponent(path.substr(p,slash-p))){*err="installed manifest contains unsafe path";return false;}p=slash+1;}
            std::vector<uint8_t> bytes;if(!ReadFileLimit(base/path,64U*1024U*1024U,&bytes)){*err="installed payload member is missing or oversized";return false;}
            if(!package->files.emplace(path,std::move(bytes)).second){*err="installed manifest contains duplicate payload path";return false;}
        }
    }
    if(!ValidatePackage(package,parsed,err)||parsed->id!=expected_id||parsed->version!=expected_version){*err="installed package identity or contents did not validate";return false;}
    return true;
}
bool InstallPackageZip(const std::string& zip_path,std::string* response,std::string* err){
    matonos_addons::VerifiedZip package;PackageModules parsed;
    if(!matonos_addons::ReadAndVerifyZip(zip_path,kRepoPublicKey,&package,err)||!ValidatePackage(&package,&parsed,err))return false;
    std::string image_hash;uint64_t image_size=0;
    if(!JsonField(package.manifest.c_str(),"imageSha256",&image_hash)||!JsonNumber(package.manifest.c_str(),"imageSize",&image_size)){
        *err="signed manifest does not identify its slot image";return false;
    }
    gPending={true,parsed.id,parsed.version,image_hash,package.manifest,package.signature,image_size};
    *response="{\"ok\":true,\"verified\":true,\"id\":"+JsonEscape(parsed.id)+",\"version\":"+JsonEscape(parsed.version)+",\"kernelRelease\":"+JsonEscape(KernelRelease())+",\"modules\":"+std::to_string(parsed.modules.size())+",\"imageBytes\":"+std::to_string(image_size)+",\"next\":\"begin_image\"}";
    return true;
}
struct SlotModule { std::string addon,version,name; };
std::vector<SlotModule> SlotModules();
bool ValidateSlotModule(const std::string& addon,const std::string& version,const std::string& name,std::string* err);
bool Loaded(const std::string& name){return fs::exists(fs::path("/sys/module")/name);}
[[maybe_unused]] bool UnloadModule(const std::string& name,std::string* err){
    if(!Loaded(name))return true;
    if(syscall(__NR_delete_module,name.c_str(),O_NONBLOCK)==0)return true;
    *err=std::string("module is busy or could not unload: ")+strerror(errno);return false;
}
bool Decode64(const std::string& in,std::vector<uint8_t>* out){static const std::string abc="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";if(in.empty()||in.size()>4*((kChunkDecoded+2)/3))return false;uint32_t acc=0;int bits=0;for(char c:in){if(c=='=')break;auto p=abc.find(c);if(p==std::string::npos)return false;acc=(acc<<6)|(uint32_t)p;bits+=6;if(bits>=8){bits-=8;out->push_back((uint8_t)(acc>>bits));}}return out->size()<=kChunkDecoded;}
int Reply(char* out,size_t cap,const std::string& value){if(value.size()+1>cap)return -1;memcpy(out,value.c_str(),value.size()+1);return 0;}
int Status(const char*,char* out,size_t cap,void*){
    const std::string release=KernelRelease();std::ostringstream s;s<<"{\"ok\":true,\"kernelRelease\":"<<JsonEscape(release)<<",\"slotMounted\":"<<(IsSlotMounted()?"true":"false")<<",\"addons\":[";
    bool first_addon=true;std::error_code ec;
    if(fs::is_directory(kSlot,ec))for(const auto& dir:fs::directory_iterator(kSlot,ec)){
        if(ec)break;const std::string addon=dir.path().filename().string();if(!dir.is_directory()||!Name(addon))continue;
        std::ifstream vf(dir.path()/"active-version");std::string version;std::getline(vf,version);version=Trim(version);if(!SafeVersion(version))continue;
        matonos_addons::VerifiedZip package;PackageModules parsed;std::string error;
        const bool valid=LoadInstalledPackage(dir.path()/"versions"/version,addon,version,&package,&parsed,&error);
        if(!first_addon)s<<',';first_addon=false;
        s<<"{\"id\":"<<JsonEscape(addon)<<",\"version\":"<<JsonEscape(version)<<",\"valid\":"<<(valid?"true":"false")<<",\"moduleCount\":"<<(valid?parsed.modules.size():0)<<",\"firmwareCount\":"<<(valid?parsed.firmware.size():0)<<",\"error\":"<<JsonEscape(error)<<"}";
    }
    s<<"],\"modules\":[";
    bool first=true;for(const auto& item:SlotModules()){std::string error;bool valid=ValidateSlotModule(item.addon,item.version,item.name,&error);if(!first)s<<',';first=false;s<<"{\"addon\":"<<JsonEscape(item.addon)<<",\"version\":"<<JsonEscape(item.version)<<",\"name\":"<<JsonEscape(item.name)<<",\"valid\":"<<(valid?"true":"false")<<",\"loaded\":"<<(Loaded(item.name)?"true":"false")<<",\"error\":"<<JsonEscape(error)<<"}";}
    s<<"]}";return Reply(out,cap,s.str());
}
int Progress(const char* operation_id,const char* phase,size_t done,size_t total,const char* module){
    if(!gIpc)return -1;std::string event="{\"operationId\":"+JsonEscape(operation_id?operation_id:"")+",\"operation\":\"install\",\"phase\":"+JsonEscape(phase)+",\"module\":"+JsonEscape(module?module:"")+",\"bytesDone\":"+std::to_string(done)+",\"bytesTotal\":"+std::to_string(total)+"}";
    return matonos_ipc_publish(gIpc,"progress",event.c_str());
}
int Available(const char*,char* out,size_t cap,void*){
    std::vector<uint8_t> key;const bool trusted=ReadFileLimit(kRepoPublicKey,32,&key)&&key.size()==32;
    return Reply(out,cap,"{\"ok\":true,\"slotMounted\":"+std::string(IsSlotMounted()?"true":"false")+",\"repositoryKeyConfigured\":"+(trusted?"true":"false")+",\"maxPackageBytes\":"+std::to_string(kMaxPackage)+"}");
}
int BeginPackage(const char* a,char* out,size_t cap,void*){
    std::string operation_id;uint64_t size=0;
    if(!JsonField(a,"operationId",&operation_id)||!Name(operation_id)||!JsonNumber(a,"size",&size)||size==0||size>kMaxPackage)
        return Reply(out,cap,"{\"ok\":false,\"error\":\"invalid operation id or package size\"}");
    std::lock_guard<std::mutex> l(gLock);if(gUpload.active)return Reply(out,cap,"{\"ok\":false,\"error\":\"another upload is active\"}");gPending={};
    const fs::path dir=fs::path(kData)/"incoming";std::error_code ec;fs::create_directories(dir,ec);if(ec)return Reply(out,cap,"{\"ok\":false,\"error\":\"cannot create upload staging directory\"}");chmod(dir.c_str(),0700);
    const std::string path=(dir/(operation_id+".zip")).string();int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return Reply(out,cap,"{\"ok\":false,\"error\":\"cannot create unique package upload\"}");
    gUpload={true,false,fd,operation_id,path,size,0};(void)Progress(operation_id.c_str(),"receiving",0,(size_t)size,"");return Reply(out,cap,"{\"ok\":true,\"chunkBytes\":32768}");
}
int BeginImage(const char* a,char* out,size_t cap,void*){
    uint64_t compressed_size=0;if(!JsonNumber(a,"size",&compressed_size)||compressed_size==0||compressed_size>kMaxPackage)
        return Reply(out,cap,"{\"ok\":false,\"error\":\"invalid compressed slot image size\"}");
    std::lock_guard<std::mutex> l(gLock);
    if(gUpload.active||!gPending.valid)return Reply(out,cap,"{\"ok\":false,\"error\":\"verify a signed package before uploading its image\"}");
    if(gPending.size!=kSlotImageBytes)return Reply(out,cap,"{\"ok\":false,\"error\":\"signed image size is not the supported GPT slot size\"}");
    const std::string path=std::string(kData)+"/incoming/slot.img";unlink(path.c_str());
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return Reply(out,cap,"{\"ok\":false,\"error\":\"cannot create staged slot image\"}");
    gUpload={true,true,fd,"slot-image",path,compressed_size,0};
    return Reply(out,cap,"{\"ok\":true,\"chunkBytes\":32768,\"expandedSize\":"+std::to_string(gPending.size)+"}");
}
int PackageChunk(const char* a,char* out,size_t cap,void*){
    std::string data; if(!JsonField(a,"data",&data))return Reply(out,cap,"{\"ok\":false,\"error\":\"missing base64 chunk\"}");
    std::vector<uint8_t> decoded;if(!Decode64(data,&decoded))return Reply(out,cap,"{\"ok\":false,\"error\":\"invalid chunk encoding\"}");
    std::lock_guard<std::mutex> l(gLock);if(!gUpload.active||gUpload.received+decoded.size()>gUpload.size)return Reply(out,cap,"{\"ok\":false,\"error\":\"no upload or declared size exceeded\"}");
    if(!WriteAll(gUpload.fd,decoded.data(),decoded.size()))return Reply(out,cap,"{\"ok\":false,\"error\":\"cannot write upload chunk\"}");
    gUpload.received+=decoded.size();(void)Progress(gUpload.operation_id.c_str(),"receiving",gUpload.received,gUpload.size,"");return Reply(out,cap,"{\"ok\":true,\"received\":"+std::to_string(gUpload.received)+"}");
}
int CommitPackage(const char*,char* out,size_t cap,void*){
    std::unique_lock<std::mutex> l(gLock);if(!gUpload.active||gUpload.received!=gUpload.size)return Reply(out,cap,"{\"ok\":false,\"error\":\"upload incomplete\"}");
    const std::string operation_id=gUpload.operation_id,path=gUpload.path;const size_t total=(size_t)gUpload.size;
    const bool synced=fsync(gUpload.fd)==0;close(gUpload.fd);gUpload={};l.unlock();
    std::string response,error;(void)Progress(operation_id.c_str(),"validating",total,total,"");
    bool ok=synced&&InstallPackageZip(path,&response,&error);if(!synced)error="cannot sync complete package upload";
    std::error_code ec;fs::remove(path,ec);
    if(!ok)response="{\"ok\":false,\"error\":"+JsonEscape(error)+"}";
    (void)Progress(operation_id.c_str(),ok?"complete":"failed",total,total,"");return Reply(out,cap,response);
}
bool WriteImageToSelectedPartition(const fs::path& image,std::string* error){
    char suffix[PROP_VALUE_MAX]{};if(__system_property_get("ro.boot.slot_suffix",suffix)<=0||(strcmp(suffix,"_a")&&strcmp(suffix,"_b"))){*error="boot slot suffix is unavailable";return false;}
    if(umount2(kSlot,0)!=0&&errno!=EINVAL&&errno!=ENOENT){*error=std::string("cannot unmount read-only add-on slot: ")+strerror(errno);return false;}
    const std::string block=std::string("/dev/block/by-name/addons")+suffix;
    // by-name entries are trusted ueventd symlinks; the dedicated SELinux
    // label and BLKGETSIZE64 check below validate their block-device target.
    int out=open(block.c_str(),O_WRONLY|O_CLOEXEC);if(out<0){*error=std::string("cannot open selected add-on block device: ")+strerror(errno);return false;}
    uint64_t capacity=0;if(ioctl(out,BLKGETSIZE64,&capacity)!=0||capacity!=kSlotImageBytes){close(out);*error="selected add-on partition does not match the supported 512 MiB slot size";return false;}
    int in=open(image.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(in<0){close(out);*error="staged slot image disappeared";return false;}
    // Channel callbacks run on a bounded binder worker stack. Keep the
    // streaming buffer small; a MiB stack allocation can crash the daemon.
    std::array<uint8_t,64*1024> buf{};uint64_t offset=0;bool ok=true;
    while(offset<kSlotImageBytes){size_t want=(size_t)std::min<uint64_t>(buf.size(),kSlotImageBytes-offset);ssize_t n=read(in,buf.data(),want);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}size_t done=0;while(done<(size_t)n){ssize_t w=pwrite(out,buf.data()+done,(size_t)n-done,(off_t)(offset+done));if(w<0&&errno==EINTR)continue;if(w<=0){ok=false;break;}done+=(size_t)w;}if(!ok)break;offset+=(uint64_t)n;}
    if(ok)ok=fsync(out)==0;close(in);close(out);sync();
    if(!ok){*error=std::string("raw slot image write failed: ")+strerror(errno);return false;}return true;
}
[[maybe_unused]] bool SavePendingManifest(std::string* error){
    const fs::path dir=fs::path(kData)/(gPending.id+"-"+gPending.version);std::error_code ec;fs::create_directories(dir,ec);
    if(ec){*error="cannot create signed add-on metadata directory";return false;}chmod(dir.c_str(),0700);
    const std::string suffix=std::to_string(getpid());const fs::path manifest_tmp=dir/(".manifest-"+suffix),sig_tmp=dir/(".signature-"+suffix);
    if(!WritePackageFile(manifest_tmp,reinterpret_cast<const uint8_t*>(gPending.manifest.data()),gPending.manifest.size(),0600)||
       !WritePackageFile(sig_tmp,gPending.signature.data(),gPending.signature.size(),0600)){unlink(manifest_tmp.c_str());unlink(sig_tmp.c_str());*error="cannot persist verified signed manifest";return false;}
    if(rename(manifest_tmp.c_str(),(dir/"manifest.json").c_str())!=0||rename(sig_tmp.c_str(),(dir/"manifest.sig").c_str())!=0){unlink(manifest_tmp.c_str());unlink(sig_tmp.c_str());*error="cannot atomically publish signed manifest";return false;}
    int fd=open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd>=0){(void)fsync(fd);close(fd);}return true;
}
int CommitImage(const char*,char* out,size_t cap,void*){
    std::unique_lock<std::mutex> l(gLock);if(!gUpload.active||!gUpload.image||!gPending.valid||gUpload.received!=gUpload.size)return Reply(out,cap,"{\"ok\":false,\"error\":\"slot image upload is incomplete\"}");
    const std::string compressed_name=std::move(gUpload.path),raw_name=compressed_name+".raw";
    const fs::path compressed(compressed_name),raw(raw_name);const bool synced=fsync(gUpload.fd)==0;close(gUpload.fd);gUpload={};l.unlock();
    std::string digest,error;bool ok=synced&&ExpandImageGzip(compressed,raw)&&HashFile(raw,kSlotImageBytes,&digest)&&digest==gPending.sha256;
    if(!synced)error="cannot sync staged slot image";else if(!ok)error="compressed image is invalid or its expanded hash does not match the signed manifest";
    if(ok)ok=WriteImageToSelectedPartition(raw,&error);
    std::error_code ec;fs::remove(compressed,ec);fs::remove(raw,ec);
    if(ok){const std::string id=gPending.id,version=gPending.version;gPending={};
        return Reply(out,cap,"{\"ok\":true,\"id\":"+JsonEscape(id)+",\"version\":"+JsonEscape(version)+",\"rebootRequired\":true,\"slotMounted\":false}");}
    gPending={};return Reply(out,cap,"{\"ok\":false,\"error\":"+JsonEscape(error.empty()?"slot image install failed":error)+"}");
}
int Uninstall(const char*,char* out,size_t cap,void*){
    return Reply(out,cap,"{\"ok\":false,\"error\":\"the mounted slot is immutable; uninstall requires a newly signed replacement slot image\"}");
}
int LoadOne(const char* a,char* out,size_t cap,void*){
    std::string name;if(!JsonField(a,"name",&name)||!Name(name))return Reply(out,cap,"{\"ok\":false,\"error\":\"invalid module name\"}");if(Loaded(name))return Reply(out,cap,"{\"ok\":true,\"loaded\":true}");
    for(const auto& item:SlotModules())if(item.name==name){std::string error;if(!ValidateSlotModule(item.addon,item.version,name,&error))return Reply(out,cap,"{\"ok\":false,\"error\":"+JsonEscape(error)+"}");bool ok=RunModprobe(item.addon,item.version,name);return Reply(out,cap,ok?"{\"ok\":true,\"loaded\":true}":"{\"ok\":false,\"error\":\"modprobe failed\"}");}
    return Reply(out,cap,"{\"ok\":false,\"error\":\"module is not in an active signed add-on\"}");
}
std::vector<SlotModule> SlotModules(){
    std::vector<SlotModule> modules;std::error_code ec;if(!fs::is_directory(kSlot,ec))return modules;
    for(const auto& dir:fs::directory_iterator(kSlot,ec)){if(ec)break;if(!dir.is_directory())continue;std::string addon=dir.path().filename().string();if(!Name(addon))continue;
        std::ifstream active(dir.path()/"active-version");std::string version;std::getline(active,version);version=Trim(version);if(!SafeVersion(version))continue;
        std::ifstream f(dir.path()/"versions"/version/"modules.load");std::string row;while(std::getline(f,row)){row=Trim(row);if(row.empty()||row[0]=='#')continue;if(row.size()>3&&row.compare(row.size()-3,3,".ko")==0)row.resize(row.size()-3);if(Name(row))modules.push_back({addon,version,row});}
    }return modules;
}
bool ValidateSlotModule(const std::string& addon,const std::string& version,const std::string& name,std::string* err){
    if(!Name(addon)||!SafeVersion(version)||!Name(name)){*err="unsafe add-on, version, or module name";return false;}
    matonos_addons::VerifiedZip package;PackageModules parsed;
    if(!LoadInstalledPackage(fs::path(kSlot)/addon/"versions"/version,addon,version,&package,&parsed,err))return false;
    if(std::find(parsed.modules.begin(),parsed.modules.end(),name)==parsed.modules.end()){*err="module is not listed by this signed add-on";return false;}
    return true;
}
bool RunModprobe(const std::string& addon,const std::string& version,const std::string& name){
    pid_t pid=fork();if(pid<0)return false;
    const std::string module_dir=(fs::path(kSlot)/addon/"versions"/version/"modules").string();
    if(pid==0){execl("/vendor/bin/modprobe","modprobe","-d",module_dir.c_str(),"-d","/vendor/lib/modules",name.c_str(),(char*)nullptr);_exit(127);}
    for(int i=0;i<100;i++){int status=0;pid_t r=waitpid(pid,&status,WNOHANG);if(r==pid)return WIFEXITED(status)&&WEXITSTATUS(status)==0;if(r<0&&errno!=EINTR)return false;usleep(100000);}
    kill(pid,SIGTERM);usleep(100000);kill(pid,SIGKILL);(void)waitpid(pid,nullptr,0);return false;
}
bool LoadSlotModules(){
    std::error_code ec;if(!fs::is_directory(kSlot,ec))return true;
    for(const auto& item:SlotModules()){std::string error;if(!ValidateSlotModule(item.addon,item.version,item.name,&error)){__android_log_print(ANDROID_LOG_WARN,kLog,"skip %s/%s: %s",item.addon.c_str(),item.name.c_str(),error.c_str());continue;}if(Loaded(item.name))continue;bool ok=RunModprobe(item.addon,item.version,item.name);__android_log_print(ok?ANDROID_LOG_INFO:ANDROID_LOG_WARN,kLog,"slot module %s/%s %s",item.addon.c_str(),item.name.c_str(),ok?"loaded":"failed (optional)");}
    return true;
}
bool Register(){gIpc=matonos_ipc_create("addons");return gIpc&&
    matonos_ipc_register(gIpc,"status",Status,nullptr)==0&&matonos_ipc_register(gIpc,"list",Status,nullptr)==0&&
    matonos_ipc_register(gIpc,"available",Available,nullptr)==0&&
    matonos_ipc_register(gIpc,"begin_package",BeginPackage,nullptr)==0&&matonos_ipc_register(gIpc,"package_chunk",PackageChunk,nullptr)==0&&
    matonos_ipc_register(gIpc,"commit_package",CommitPackage,nullptr)==0&&matonos_ipc_register(gIpc,"begin_image",BeginImage,nullptr)==0&&
    matonos_ipc_register(gIpc,"commit_image",CommitImage,nullptr)==0&&matonos_ipc_register(gIpc,"uninstall",Uninstall,nullptr)==0&&
    matonos_ipc_register(gIpc,"remove",Uninstall,nullptr)==0&&matonos_ipc_register(gIpc,"load",LoadOne,nullptr)==0&&matonos_ipc_start(gIpc)==0;}
}

namespace matonos_addons {
std::string Sha256Hex(const std::vector<uint8_t>& bytes) { return Hash(bytes); }
}  // namespace matonos_addons

int main(){
    if(!Register()){__android_log_print(ANDROID_LOG_ERROR,kLog,"channel registration failed; boot continues without add-on control");return 1;}
    (void)LoadSlotModules(); // optional list; each modprobe is time-bounded.
    __android_log_print(ANDROID_LOG_INFO,kLog,"registered addons channel for kernel %s",KernelRelease().c_str());
    for(;;)pause();
}
