// Bluetooth controller choice only: no transport, virtual controller or HAL.
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <chrono>
#include "matonos_ipc.h"
#define INFO(...) __android_log_print(ANDROID_LOG_INFO,"matonos-btd",__VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR,"matonos-btd",__VA_ARGS__)
#define CLOSE(fd) do { INFO("close fd=%d at %s:%d",fd,__FILE__,__LINE__); if(close(fd)<0) ERR("close fd=%d errno=%d (%s)",fd,errno,strerror(errno)); } while(0)
static const char *choiceKey="persist.vendor.maton.bluetooth.device";
static const char *cachePath="/data/vendor/maton-btd/devices";
struct Device { std::string address, path, usb, vendor, product, generation, name, bus, hci; bool bound=false; };
static std::vector<Device> devices;
static std::mutex lock;
static MatonosIpcServer *server;
static std::string preferred;
static std::string prop(const char *key) { char v[PROP_VALUE_MAX]={}; __system_property_get(key,v); return v; }
static std::string readText(const std::string& path) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC); if(fd<0) { if(errno!=ENOENT) ERR("open %s errno=%d (%s)",path.c_str(),errno,strerror(errno)); return ""; }
    std::string s; char b[4096]; ssize_t n;
    while((n=read(fd,b,sizeof(b)))!=0) { if(n<0) { if(errno==EINTR) continue; ERR("read %s fd=%d errno=%d",path.c_str(),fd,errno); break; } s.append(b,n); }
    CLOSE(fd); while(!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); return s;
}
static bool writeText(const std::string& path,const std::string& text,int flags=O_WRONLY) {
    int fd=open(path.c_str(),flags|O_CLOEXEC,0600); if(fd<0) { ERR("open write %s errno=%d (%s)",path.c_str(),errno,strerror(errno)); return false; }
    size_t pos=0; while(pos<text.size()) { ssize_t n=write(fd,text.data()+pos,text.size()-pos); if(n<0 && errno==EINTR) continue; if(n<=0) { ERR("write %s fd=%d errno=%d (%s)",path.c_str(),fd,errno,strerror(errno)); CLOSE(fd); return false; } pos+=n; }
    CLOSE(fd); return true;
}
static std::string real(const std::string& p) { char b[PATH_MAX]; if(realpath(p.c_str(),b)) return b; if(errno!=ENOENT) ERR("realpath %s errno=%d",p.c_str(),errno); return ""; }
static std::string quote(const std::string& s) { std::string q="\""; for(unsigned char c:s) { if(c=='"'||c=='\\') q+='\\'; if(c>=32) q+=c; } return q+'"'; }
static bool addressValid(const std::string& a) { if(a.size()!=17) return false; for(size_t i=0;i<a.size();i++) if(i%3==2 ? a[i]!=':' : !isxdigit(static_cast<unsigned char>(a[i]))) return false; return a!="00:00:00:00:00:00"; }
// READ_INFO returns the public address even with Powered clear. USB descriptors
// do not contain a reliable Bluetooth address; retain identities before unbind.
static bool info(unsigned index,Device& d) {
    struct { sa_family_t family; unsigned short dev,channel; } sa={AF_BLUETOOTH,0xffff,3};
    int fd=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,1);
    if(fd<0) { ERR("MGMT socket hci%u errno=%d",index,errno); return false; }
    bool ok=false;
    if(bind(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))<0) ERR("MGMT bind fd=%d hci%u errno=%d",fd,index,errno);
    else {
        unsigned char cmd[]={4,0,static_cast<unsigned char>(index),static_cast<unsigned char>(index>>8),0,0};
        if(write(fd,cmd,sizeof(cmd))!=sizeof(cmd)) ERR("MGMT READ_INFO write fd=%d hci%u errno=%d",fd,index,errno);
        else {
            pollfd p={fd,POLLIN,0};
            auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
            for(;;) {
                if(std::chrono::steady_clock::now()>=deadline) { ERR("MGMT READ_INFO deadline hci%u fd=%d",index,fd); break; }
                int r=poll(&p,1,500); if(r<0 && errno==EINTR) continue;
                if(r<=0) { ERR("MGMT READ_INFO poll fd=%d hci%u result=%d errno=%d",fd,index,r,errno); break; }
                unsigned char b[1024]; ssize_t n=read(fd,b,sizeof(b));
                if(n<0 && (errno==EINTR||errno==EAGAIN)) continue;
                if(n<0) { ERR("MGMT read fd=%d errno=%d",fd,errno); break; }
                if(n<9 || b[0]!=1 || b[1]!=0 || b[2]!=cmd[2] || b[3]!=cmd[3] || b[6]!=4 || b[7]!=0) continue;
                if(b[8]!=0 || n<289) { ERR("MGMT READ_INFO hci%u status=%u bytes=%zd",index,b[8],n); break; }
                char a[18]; snprintf(a,sizeof(a),"%02X:%02X:%02X:%02X:%02X:%02X",b[14],b[13],b[12],b[11],b[10],b[9]); d.address=a;
                d.name=std::string(reinterpret_cast<char*>(b+29),strnlen(reinterpret_cast<char*>(b+29),249)); ok=addressValid(d.address); break;
            }
        }
    }
    CLOSE(fd); return ok;
}
static void scan() {
    for(auto& d:devices) { d.bound=false; d.hci.clear(); }
    DIR *dir=opendir("/sys/class/bluetooth");
    if(!dir) { if(errno!=ENOENT) ERR("opendir bluetooth errno=%d",errno); return; }
    while(auto *e=readdir(dir)) {
        unsigned index; char tail; if(sscanf(e->d_name,"hci%u%c",&index,&tail)!=1) continue;
        Device d; d.hci=e->d_name; d.bound=true; d.path=real("/sys/class/bluetooth/"+d.hci+"/device"); d.bus="other";
        std::string parent=d.path;
        while(!parent.empty() && parent!="/sys") {
            if(real(parent+"/driver")=="/sys/bus/usb/drivers/btusb") {
                d.usb=parent.substr(parent.rfind('/')+1); d.bus="USB";
                auto dev=parent.substr(0,parent.rfind('/')); d.vendor=readText(dev+"/idVendor"); d.product=readText(dev+"/idProduct"); d.generation=readText(dev+"/devnum"); break;
            }
            if(parent.find("/serial")!=std::string::npos) d.bus="UART";
            parent=parent.substr(0,parent.rfind('/'));
        }
        if(!info(index,d)) { auto old=std::find_if(devices.begin(),devices.end(),[&](const Device& v){return v.path==d.path;}); if(old!=devices.end()) { d.address=old->address; d.name=old->name; } }
        auto old=std::find_if(devices.begin(),devices.end(),[&](const Device& v){return v.path==d.path;});
        if(old==devices.end()) devices.push_back(d); else *old=d;
    }
    INFO("closedir at %s:%d",__FILE__,__LINE__); if(closedir(dir)<0) ERR("closedir bluetooth errno=%d",errno);
    // USB device numbers change on replug. Never reuse an unbound identity
    // for a replacement adapter that happened to occupy the same port.
    devices.erase(std::remove_if(devices.begin(),devices.end(),[](const Device& d){
        if(d.bound) return false;
        if(d.usb.empty() || access(("/sys/bus/usb/devices/"+d.usb).c_str(),F_OK)!=0) return true;
        auto parent=real("/sys/bus/usb/devices/"+d.usb); parent=parent.substr(0,parent.rfind('/'));
        return d.generation.empty() || readText(parent+"/devnum")!=d.generation;
    }),devices.end());
}
static bool saveCache() {
    std::string s; for(const auto& d:devices) if(!d.usb.empty() && addressValid(d.address)) s+=d.address+" "+d.usb+" "+d.vendor+" "+d.product+" "+d.generation+" "+d.path+"\n";
    if(!writeText(std::string(cachePath)+".tmp",s,O_WRONLY|O_CREAT|O_TRUNC)) return false;
    if(rename((std::string(cachePath)+".tmp").c_str(),cachePath)<0) { ERR("rename cache errno=%d",errno); return false; } return true;
}
static std::string snapshot() {
    std::string s="{\"devices\":["; bool first=true;
    for(const auto& d:devices) { if(!first) s+=','; first=false;
        s+="{\"address\":"+quote(d.address)+",\"perm_mac\":"+quote(d.address)+",\"interface\":"+quote(d.hci)+",\"device\":"+quote(d.path)+",\"bus\":"+quote(d.bus)+",\"usb_id\":"+quote(d.vendor+":"+d.product)+",\"name\":"+quote(d.name)+",\"bound\":"+(d.bound?"true":"false")+",\"selected\":"+(d.address==preferred?"true":"false")+"}";
    } return s+"]}";
}
static bool apply(const std::string& address) {
    auto chosen=std::find_if(devices.begin(),devices.end(),[&](const Device& d){return d.address==address;});
    if(chosen==devices.end()) { ERR("select address=%s unavailable",address.c_str()); return false; }
    Device selected=*chosen;
    for(const auto& d:devices) if(d.bound && !addressValid(d.address)) { ERR("refusing switch: unknown identity hci=%s",d.hci.c_str()); return false; }
    if(!saveCache()) return false; // Never lose the address of a controller we detach.
    for(const auto& d:devices) if(d.address!=address && d.bound) {
        if(d.usb.empty()) { ERR("cannot unbind non-USB controller %s path=%s",d.address.c_str(),d.path.c_str()); continue; }
        if(!writeText("/sys/bus/usb/drivers/btusb/unbind",d.usb)) return false;
    }
    if(!selected.bound && !selected.usb.empty() && !writeText("/sys/bus/usb/drivers/btusb/bind",selected.usb)) return false;
    return true;
}
static int copy(const std::string& s,char *out,size_t n) { if(s.size()+1>n) { ERR("IPC response needs %zu capacity=%zu",s.size()+1,n); return -1; } memcpy(out,s.c_str(),s.size()+1); return 0; }
static int state(const char*,char *out,size_t n,void*) { std::lock_guard<std::mutex> guard(lock); return copy("{\"selected\":"+quote(preferred)+"}",out,n); }
static int list(const char*,char *out,size_t n,void*) { std::lock_guard<std::mutex> guard(lock); scan(); return copy(snapshot(),out,n); }
static int selectDevice(const char *args,char *out,size_t n,void*) {
    std::lock_guard<std::mutex> guard(lock); std::string s=args?args:"";
    auto k=s.find("\"device\""); auto c=k==s.npos?s.npos:s.find(':',k+8); auto a=c==s.npos?s.npos:s.find('"',c+1); auto b=a==s.npos?s.npos:s.find('"',a+1);
    std::string address=b==s.npos?"":s.substr(a+1,b-a-1); for(char& v:address) v=toupper(static_cast<unsigned char>(v));
    if(!addressValid(address)) { ERR("select_device invalid address"); return -1; }
    bool persist=true;
    auto pk=s.find("\"persist\"");
    if(pk!=s.npos) {
        auto pc=s.find(':',pk+9); auto pv=pc==s.npos?s.npos:s.find_first_not_of(" \t\r\n",pc+1);
        if(pv==s.npos) { ERR("select_device missing persist value"); return -1; }
        if(s.compare(pv,4,"true")==0) persist=true;
        else if(s.compare(pv,5,"false")==0) persist=false;
        else { ERR("select_device invalid persist value"); return -1; }
    }
    scan(); if(!apply(address)) return -1;
    if(persist && __system_property_set(choiceKey,address.c_str())<0) { ERR("persist address=%s errno=%d",address.c_str(),errno); return -1; } if(persist) preferred=address;
    scan(); return copy("{\"selected\":"+quote(address)+"}",out,n);
}
int main(int argc,char **argv) {
    preferred=prop(choiceKey);
    if(argc>1 && strcmp(argv[1],"--boot")==0 && preferred.empty()) return 0;
    std::string cache=readText(cachePath); size_t pos=0;
    while(pos<cache.size()) { size_t end=cache.find('\n',pos); if(end==cache.npos) end=cache.size(); std::string line=cache.substr(pos,end-pos); char a[18],u[PATH_MAX],v[16],p[16],generation[32],path[PATH_MAX];
        if(sscanf(line.c_str(),"%17s %4095s %15s %15s %31s %4095s",a,u,v,p,generation,path)==6 && addressValid(a)) { Device d; d.address=a; d.usb=u; d.vendor=v; d.product=p; d.generation=generation; d.path=path; d.bus="USB"; devices.push_back(d); } else ERR("invalid cached device record"); pos=end+1;
    }
    scan();
    if(argc>1 && strcmp(argv[1],"--boot")==0) { if(!preferred.empty()) apply(preferred); return 0; }
    server=matonos_ipc_create("bluetooth");
    if(!server || matonos_ipc_register(server,"get_state",state,nullptr)!=0 || matonos_ipc_register(server,"list_devices",list,nullptr)!=0 || matonos_ipc_register(server,"select_device",selectDevice,nullptr)!=0 || matonos_ipc_start(server)!=0) { ERR("bluetooth IPC startup failed"); return 1; }
    std::string previous=snapshot();
    for(;;) { sleep(1); std::lock_guard<std::mutex> guard(lock); scan(); std::string now=snapshot(); if(now!=previous) {
        // Bridge performs powered-down reconciliation on hotplug.
        if(matonos_ipc_publish(server,"devices_changed",now.c_str())<0) ERR("publish devices_changed failed"); previous=now;
    } }
}
