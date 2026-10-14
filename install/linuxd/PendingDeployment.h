#ifndef MATON_PENDING_DEPLOYMENT_H
#define MATON_PENDING_DEPLOYMENT_H
#include "DeploymentStore.h"
#include "RemotePin.h"
/* Private unpublished versions are named by Flatpak application ID. They are
 * never launcher pointers, and can only be handed out through an operation. */
static inline int pending_deployment_path(const char* path) {
    const char* prefix=MATON_INSTALL_ROOT "/.pending-";
    if(!path || strncmp(path,prefix,strlen(prefix)))return 0;
    const char* name=path+strlen(prefix),*dash=strrchr(name,'-');
    if(!dash || dash==name || strlen(dash+1)!=6 || (size_t)(dash-name)>220)return 0;
    for(const char* p=name;p<dash;p++)
        if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='_'||*p=='-'))return 0;
    for(const char* p=dash+1;*p;p++)
        if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')))return 0;
    return 1;
}
static inline int pending_deployment_discard(const char* path) {
    if(!pending_deployment_path(path))return deployment_discard(path);
    int parent=open(MATON_INSTALL_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(parent<0)return -1;
    const char* name=strrchr(path,'/')+1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){int rc=errno==ENOENT?0:-1;close(parent);return rc;}
    struct stat st,up;
    int rc=fstat(fd,&st)||fstat(parent,&up)||st.st_uid!=1000||st.st_dev!=up.st_dev;
    if(!rc)rc=deployment_empty(fd,st.st_dev);
    if(!rc)rc=unlinkat(parent,name,AT_REMOVEDIR);
    close(fd);close(parent);return rc;
}
static inline int pending_deployment_create(const char* ref,char* path,size_t size) {
    char app[256];
    if(remote_pin_name(ref,app,sizeof(app)) || strlen(app)>220 || maton_store_prepare())return -1;
    int n=snprintf(path,size,MATON_INSTALL_ROOT "/.pending-%s-XXXXXX",app);
    if(n<0||(size_t)n>=size || maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))return -1;
    char* made=mkdtemp(path);int clear=maton_storage_context(NULL);
    if(!made)return -1;
    if(clear || chmod(path,0755))goto bad;
    char lease[PATH_MAX];snprintf(lease,sizeof(lease),"%s/.lease",path);
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))goto bad;
    int fd=open(lease,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    clear=maton_storage_context(NULL);
    if(fd<0)goto bad;
    int rc=clear || fchmod(fd,0644) || fsync(fd);close(fd);
    if(!rc)return 0;
bad:
    pending_deployment_discard(path);return -1;
}
#endif
