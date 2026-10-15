#ifndef MATON_REPO_MANAGEMENT_H
#define MATON_REPO_MANAGEMENT_H
/* Included by FlatpakManager.c: all mutations hold its publication mutex. */
static int repo_name_valid(const char* name) {
    if(!name || !*name || strlen(name)>128 || name[0]=='-' || name[0]=='.')return 0;
    for(const unsigned char* p=(const unsigned char*)name;*p;p++)
        if(!isalnum(*p) && *p!='.' && *p!='_' && *p!='-')return 0;
    return 1;
}
/* Only the canonical three-field file from the bridge is accepted. This
 * independently prevents unsigned repos and Flatpak option injection. */
static int repo_file_valid(const char* text,char* url,size_t size) {
    if(!text || strlen(text)>65536 || strncmp(text,"[Flatpak Repo]\nTitle=",21))return 0;
    const char* title=text+21; const char* u=strchr(title,'\n');
    if(!u || u==title || strncmp(u,"\nUrl=https://",13))return 0;
    const char* start=u+5;const char* key=strchr(start,'\n');
    if(!key || strncmp(key,"\nGPGKey=",8) || (size_t)(key-start)>=size)return 0;
    memcpy(url,start,(size_t)(key-start));url[key-start]=0;
    const char* host=start+8;const char* slash=memchr(host,'/',(size_t)(key-host));
    if(!slash || slash==host)return 0;
    for(const char* p=host;p<slash;p++)if(!isalnum((unsigned char)*p) && *p!='.' && *p!='-' && *p!=':')return 0;
    for(const char* p=start;p<key;p++)if((unsigned char)*p<=32 || *p=='\\' || *p=='@' || *p=='?' || *p=='#')return 0;
    const char* encoded=key+8;const char* key_end=strchr(encoded,'\n');
    if(!key_end)return 0;
    size_t n=(size_t)(key_end-encoded);
    if(n<4 || n%4)return 0;
    for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)encoded[i]) && encoded[i]!='+' && encoded[i]!='/' && encoded[i]!='=')return 0;
    const char* extra=key_end+1;
    while(*extra) {
        const char* end=strchr(extra,'\n');if(!end)return 0;
        if(strncmp(extra,"Homepage=",9) && strncmp(extra,"Comment=",8) && strncmp(extra,"Icon=",5))return 0;
        for(const char* p=extra;p<end;p++)if((unsigned char)*p<32 || *p=='\\')return 0;
        extra=end+1;
    }
    for(const char* p=title;p<u;p++)if((unsigned char)*p<32 || *p=='\\')return 0;
    return 1;
}
static int repo_drop_pins(const char* remote) {
    int fd=open(MATON_LINUX_ROOT "/manager",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    DIR* dir=fdopendir(fd);if(!dir){close(fd);return -1;}
    struct dirent* e;int rc=0;
    while((e=readdir(dir))) {
        char value[130];int pin=openat(fd,e->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(pin<0)continue;
        struct stat st;ssize_t n=-1;
        if(!fstat(pin,&st) && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<=128)n=read(pin,value,129);
        close(pin);
        if(n>0 && n<=128){value[n]=0;if(!strcmp(value,remote) && unlinkat(fd,e->d_name,0))rc=-1;}
    }
    if(fsync(fd))rc=-1;
    closedir(dir);return rc;
}
/* Do not remove trust while a current deployment or unpublished PM handoff
 * still needs it. Immutable leased old versions are retained by normal GC. */
static int repo_has_apps(const char* remote) {
    DIR* dir=opendir(MATON_INSTALL_ROOT);if(!dir)return -1;
    struct dirent* e;int rc=0;
    while((e=readdir(dir))) {
        char* end;long id=strtol(e->d_name,&end,10);if(*end || id<10000 || id>19999)continue;
        char path[512];snprintf(path,sizeof(path),"%s/%s",MATON_INSTALL_ROOT,e->d_name);
        const char* args[]={"list","--user","--app","--columns=application,origin"};
        ChildResult child=run_cli_in(args,4,0,MATON_RUNTIME_DIR,path);
        if(child.status || child.truncated){free(child.output);rc=-1;break;}
        char* save=NULL;
        for(char* line=strtok_r(child.output,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
            char* tab=strchr(line,'\t');if(!tab){rc=-1;break;}
            if(!strcmp(tab+1,remote)){rc=1;break;}
        }
        free(child.output);if(rc)break;
    }
    closedir(dir);return rc;
}
static void repo_manage(const char* command,const char* file,const char* remote,FlatpakResult* result) {
    if(!repo_name_valid(remote) || !strcmp(remote,"flathub")){set_error(result,"Invalid repository name or protected built-in flathub");return;}
    if(maton_storage_prepare()){set_error(result,"Linux app storage unavailable");return;}
    if(pthread_mutex_trylock(&g_operation_mutex)){set_error(result,"A Flatpak operation is running; retry later");return;}
    if(pending_install.operation[0]){set_error(result,"A deployment awaits PackageManager binding");goto done;}
    if(!strcmp(command,"install_repo")) {
        char url[4096];
        if(!repo_file_valid(file,url,sizeof(url))){set_error(result,"Invalid signed HTTPS repository file");goto done;}
        const char* list[]={"remotes","--system","--columns=name,url"};
        ChildResult existing=run_cli_in(list,3,0,MATON_RUNTIME_DIR,NULL);
        int duplicate=0;
        if(existing.status || existing.truncated){free(existing.output);set_error(result,"Cannot inspect configured repositories");goto done;}
        char* save=NULL;
        for(char* line=strtok_r(existing.output,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
            char* tab=strchr(line,'\t');if(!tab){duplicate=-1;break;}*tab++=0;
            if(!strcmp(line,remote) || !strcmp(tab,url)){duplicate=1;break;}
        }
        free(existing.output);
        if(duplicate){set_error(result,duplicate>0?"Repository name or URL already configured":"Invalid repository inventory");goto done;}
        char path[]=MATON_LINUX_ROOT "/manager" "/tmp/repo-XXXXXX";
        int fd=mkstemp(path);
        if(fd<0){set_error(result,"Cannot stage repository file");goto done;}
        size_t size=strlen(file);int bad=fchmod(fd,0600) || write(fd,file,size)!=(ssize_t)size || fsync(fd);close(fd);
        if(bad){unlink(path);set_error(result,"Cannot write repository file");goto done;}
        const char* args[]={"remote-add","--system","--from",remote,path};
        ChildResult child=run_cli_in(args,5,0,MATON_RUNTIME_DIR,NULL);unlink(path);
        result_from_child(result,&child);free(child.output);
        /* Flatpak --from defaults to summary verification; verify both persisted
         * flags with the same trust-root check publication uses. */
        if(!result->ok || !flatpak_remote_verified(remote)) {
            const char* rollback[]={"remote-delete","--system",remote};
            ChildResult undo=run_cli_in(rollback,3,0,MATON_RUNTIME_DIR,NULL);free(undo.output);
            if(result->ok)set_error(result,"Repository must verify commits and summaries");
        }
    } else {
        int apps=repo_has_apps(remote);
        if(apps){set_error(result,apps>0?"Repository still has installed applications":"Cannot verify repository applications");goto done;}
        const char* args[]={"remote-delete","--system","--force",remote};
        ChildResult child=run_cli_in(args,4,0,MATON_RUNTIME_DIR,NULL);
        result_from_child(result,&child);free(child.output);
        if(result->ok) {
            char key[512];snprintf(key,sizeof(key),MATON_RUNTIME_DIR "/repo/%s.trustedkeys.gpg",remote);
            if((unlink(key) && errno!=ENOENT) || repo_drop_pins(remote))set_error(result,"Repository removed; key/pin cleanup needs retry");
        }
    }
done:
    pthread_mutex_unlock(&g_operation_mutex);
}
#endif
