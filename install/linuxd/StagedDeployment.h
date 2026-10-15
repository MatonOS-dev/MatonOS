#ifndef MATON_STAGED_DEPLOYMENT_H
#define MATON_STAGED_DEPLOYMENT_H
/* Flatpak layout/bookkeeping construction from VERIFIED inputs only.
 * No producer config, deploy GVariant, overrides, active links or exports are
 * imported. The caller verifies all dependencies before calling this helper.
 * This header keeps linuxd's fixed Soong source set unchanged. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "LinuxStorage.h"

#define SD_LIMIT 131072
struct sd_bytes { unsigned char data[SD_LIMIT]; size_t size; int bad; };
static inline void sd_add(struct sd_bytes* b,const void* p,size_t n) {
    if(b->bad || n>SD_LIMIT-b->size){b->bad=1;return;}
    memcpy(b->data+b->size,p,n);b->size+=n;
}
static inline void sd_pad(struct sd_bytes* b,size_t align) {
    unsigned char zero[8]={0};sd_add(b,zero,(align-b->size%align)%align);
}
static inline size_t sd_width(size_t payload,size_t offsets) {
    size_t w=1;
    while(w<8 && payload+offsets*w> (w==1?255:w==2?65535:UINT32_MAX))w*=2;
    return w;
}
static inline void sd_offset(struct sd_bytes* b,size_t v,size_t width) {
    unsigned char bytes[8];for(size_t i=0;i<width;i++)bytes[i]=(unsigned char)((uint64_t)v>>(8*i));
    sd_add(b,bytes,width);
}
static inline void sd_string(struct sd_bytes* b,const char* value) {sd_add(b,value,strlen(value)+1);}
/* Add a normal-form {sv} entry. Entry offsets and all GVariant framing
 * integers are little endian on MatonOS's x86_64 target. */
static inline void sd_dict_entry(struct sd_bytes* array,const char* key,const char* text,int integer) {
    struct sd_bytes* entry=calloc(1,sizeof(*entry));
    if(!entry){array->bad=1;return;}
    sd_string(entry,key);size_t key_end=entry->size;sd_pad(entry,8);
    if(text){sd_string(entry,text);sd_add(entry,"\0s",2);}
    else {unsigned char value[6]={(unsigned char)integer,0,0,0,0,'i'};sd_add(entry,value,sizeof(value));}
    sd_offset(entry,key_end,sd_width(entry->size,1));
    sd_pad(array,8);sd_add(array,entry->data,entry->size);if(entry->bad)array->bad=1;
    free(entry);
}
/* Construct (ssasta{sv}), the public on-disk Flatpak deployment format.
 * Version4's runtime and extension relation come from authenticated metadata;
 * timestamp0 has Flatpak's documented "unknown timestamp" meaning. */
static inline int sd_deploy_bytes(struct sd_bytes* out,const char* origin,const char* commit,
        const char* const* subpaths,size_t count,const char* runtime,const char* extension,uint64_t size) {
    if(count>256)return -1;
    size_t ends[256],fields[3],dict_ends[4],dict_count=0;
    struct sd_bytes* array=calloc(1,sizeof(*array));
    struct sd_bytes* dict=calloc(1,sizeof(*dict));
    if(!array||!dict){free(array);free(dict);return -1;}
    for(size_t i=0;i<count;i++){sd_string(array,subpaths[i]);ends[i]=array->size;}
    size_t w=sd_width(array->size,count);
    for(size_t i=0;i<count;i++)sd_offset(array,ends[i],w);
    sd_dict_entry(dict,"deploy-version",NULL,4);dict_ends[dict_count++]=dict->size;
    if(runtime && *runtime){sd_dict_entry(dict,"runtime",runtime,0);dict_ends[dict_count++]=dict->size;}
    if(extension && *extension){sd_dict_entry(dict,"extension-of",extension,0);dict_ends[dict_count++]=dict->size;}
    w=sd_width(dict->size,dict_count);
    for(size_t i=0;i<dict_count;i++)sd_offset(dict,dict_ends[i],w);
    sd_string(out,origin);fields[0]=out->size;
    sd_string(out,commit);fields[1]=out->size;
    sd_add(out,array->data,array->size);fields[2]=out->size;
    sd_pad(out,8);
    unsigned char be[8];for(size_t i=0;i<8;i++)be[i]=(unsigned char)(size>>(56-8*i));
    sd_add(out,be,8);sd_pad(out,8);sd_add(out,dict->data,dict->size);
    w=sd_width(out->size,3);
    for(int i=2;i>=0;i--)sd_offset(out,fields[i],w);
    int rc=out->bad || array->bad || dict->bad?-1:0;free(array);free(dict);return rc;
}
/* Parse only two public keys, never copy arbitrary producer deploy metadata.
 * The file has already passed exact signed tree comparison. */
static inline int sd_metadata(int dir,char* runtime,size_t rn,char* extension,size_t en) {
    int fd=openat(dir,"metadata",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -1;
    FILE* file=fdopen(fd,"r");if(!file){close(fd);return -1;}
    char* line=NULL;size_t cap=0,total=0;int section=0,rc=0;
    runtime[0]=extension[0]=0;
    ssize_t len;
    while((len=getline(&line,&cap,file))>=0) {
        total+=(size_t)len;if(total>SD_LIMIT || memchr(line,0,(size_t)len)){rc=-1;break;}
        while(len && (line[len-1]=='\n'||line[len-1]=='\r'))line[--len]=0;
        if(line[0]=='['){section=!strcmp(line,"[Application]")?1:!strcmp(line,"[ExtensionOf]")?2:0;continue;}
        char* value=NULL;char* dest=NULL;size_t n=0;
        if(section==1 && !strncmp(line,"runtime=",8)){value=line+8;dest=runtime;n=rn;}
        if(section==2 && !strncmp(line,"ref=",4)){value=line+4;dest=extension;n=en;}
        if(value){if(*dest || !*value || strlen(value)>=n){rc=-1;break;}strcpy(dest,value);}
    }
    if(ferror(file))rc=-1;
    free(line);if(fclose(file))rc=-1;return rc;
}
static inline int sd_tree_size(int dir,unsigned depth,uint64_t* bytes) {
    if(depth>128)return -1;
    int scan=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(scan<0)return -1;
    DIR* d=fdopendir(scan);if(!d){close(scan);return -1;}
    struct dirent* e;int rc=0;
    while(!rc && (e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
        if(S_ISREG(st.st_mode)){
            if(st.st_size<0 || (uint64_t)st.st_size>UINT64_MAX-*bytes){rc=-1;break;}
            *bytes+=(uint64_t)st.st_size;
        } else if(S_ISDIR(st.st_mode)) {
            int child=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            if(child<0){rc=-1;break;}rc=sd_tree_size(child,depth+1,bytes);close(child);
        } else if(!S_ISLNK(st.st_mode))rc=-1;
    }
    closedir(d);return rc;
}
static inline int sd_child(int parent,const char* name) {
    if(mkdirat(parent,name,0755) && errno!=EEXIST)return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat st;
    if(fstat(fd,&st)||st.st_uid!=getuid()||(st.st_mode&0022)||fchmod(fd,0755)||fsync(fd)||fsync(parent)){close(fd);errno=EPERM;return -1;}
    return fd;
}
static inline int sd_link(int dir,const char* name,const char* target) {
    char temporary[96];
    if(snprintf(temporary,sizeof(temporary),".%s-verified",name)>=(int)sizeof(temporary))return -1;
    if(unlinkat(dir,temporary,0) && errno!=ENOENT)return -1;
    if(symlinkat(target,dir,temporary)||renameat(dir,temporary,dir,name)||fsync(dir))return -1;
    return 0;
}
/* EEXIST is deliberately an error: only the verifier may authenticate and
 * choose an existing sealed runtime as a deduplication source. */
static inline int maton_staged_deploy(const char* verified,const char* target,const char* ref,
        const char* commit,const char* remote,const char* const* subpaths,size_t count,
        char* error,size_t error_size) {
    int rc=-1,root=-1,base=-1,dir=-1,source_parent=-1,source=-1,id_dir=-1;
    char parsed[513],deploy_name[NAME_MAX+1],source_path[PATH_MAX],runtime[513],extension[513];
    if(!ref||strlen(ref)>=sizeof(parsed)||!commit||strlen(commit)!=64||!remote||!*remote||strlen(remote)>128||count>256)goto done;
    strcpy(parsed,ref);char* part[4];char* save=NULL;size_t pieces=0;
    for(char* p=strtok_r(parsed,"/",&save);p;p=strtok_r(NULL,"/",&save)) {
        if(pieces==4||!strcmp(p,".")||!strcmp(p,".."))goto done;
        for(const char* c=p;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='_'||*c=='-'))goto done;
        part[pieces++]=p;
    }
    if(pieces!=4 || (strcmp(part[0],"app")&&strcmp(part[0],"runtime")))goto done;
    char canonical[513];
    if(snprintf(canonical,sizeof(canonical),"%s/%s/%s/%s",part[0],part[1],part[2],part[3])>=(int)sizeof(canonical)||strcmp(canonical,ref))goto done;
    for(const char* c=commit;*c;c++)if(!((*c>='a'&&*c<='f')||(*c>='0'&&*c<='9')))goto done;
    snprintf(deploy_name,sizeof(deploy_name),"%s",commit);size_t used=strlen(deploy_name);
    for(size_t i=0;i<count;i++) {
        if(!subpaths || !subpaths[i] || !*subpaths[i] || used+1>=sizeof(deploy_name))goto done;
        deploy_name[used++]='-';
        for(const char* c=subpaths[i];*c;c++)if(*c!='/') {
            if(used+1>=sizeof(deploy_name)||!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='_'||*c=='-'||*c=='.'))goto done;
            deploy_name[used++]=*c;
        }
        deploy_name[used]=0;
    }
    if(!verified || strlen(verified)>=sizeof(source_path))goto done;
    strcpy(source_path,verified);char* slash=strrchr(source_path,'/');
    if(!slash||!slash[1])goto done;
    *slash=0;
    source_parent=open(source_path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(source_parent<0)goto done;
    source=openat(source_parent,slash+1,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(source<0)goto done;
    struct stat st;if(fstat(source,&st)||st.st_uid!=getuid()||(st.st_mode&0022))goto done;
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))goto done;
    if(unlinkat(source,"deploy",0)&&errno!=ENOENT)goto done;
    if(sd_metadata(source,runtime,sizeof(runtime),extension,sizeof(extension)))goto done;
    uint64_t bytes=0;if(sd_tree_size(source,0,&bytes))goto done;
    struct sd_bytes* encoded=calloc(1,sizeof(*encoded));if(!encoded)goto done;
    int bad=sd_deploy_bytes(encoded,remote,commit,subpaths,count,runtime,extension,bytes);
    int file=bad?-1:openat(source,"deploy",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0644);
    if(file>=0) {
        size_t off=0;
        while(off<encoded->size){ssize_t n=write(file,encoded->data+off,encoded->size-off);if(n<0&&errno==EINTR)continue;if(n<=0){bad=1;break;}off+=(size_t)n;}
        if(fchmod(file,0644)||fsync(file))bad=1;
        if(close(file))bad=1;
    } else bad=1;
    free(encoded);if(bad||fchmod(source,0755)||fsync(source))goto done;
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))goto done;
    root=open(target,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)goto done;
    if(fstat(root,&st)||st.st_uid!=getuid()||(st.st_mode&0022))goto done;
    base=dup(root);if(base<0)goto done;
    for(size_t i=0;i<4;i++) {
        dir=sd_child(base,part[i]);if(dir<0)goto done;
        close(base);base=dir;dir=-1;
        if(i==1 && !strcmp(part[0],"app")){id_dir=dup(base);if(id_dir<0)goto done;}
    }
    if(syscall(SYS_renameat2,source_parent,slash+1,base,deploy_name,1 /* RENAME_NOREPLACE */))goto done;
    if(fsync(source_parent)||fsync(base)||sd_link(base,"active",deploy_name))goto done;
    if(id_dir>=0) {
        char current[1027];
        if(snprintf(current,sizeof(current),"%s/%s",part[2],part[3])>=(int)sizeof(current)||sd_link(id_dir,"current",current))goto done;
    }
    rc=0;
done:
    {
        int saved=errno;
        if(maton_storage_context(NULL))rc=-1;
        if(id_dir>=0)close(id_dir);
        if(source>=0)close(source);
        if(source_parent>=0)close(source_parent);
        if(dir>=0)close(dir);
        if(base>=0)close(base);
        if(root>=0)close(root);
        if(rc && error && error_size)snprintf(error,error_size,"cannot publish verified Flatpak deployment: %s",strerror(saved?saved:EINVAL));
        errno=saved;
    }
    return rc;
}
#endif
