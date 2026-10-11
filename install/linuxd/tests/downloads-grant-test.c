#define _GNU_SOURCE
#include <assert.h>
#include "../DownloadsGrant.h"
#include "../DownloadsMedia.h"
#include "../DownloadsUserDirs.h"

static void metadata(void) {
    MatonFolderRequests folders={0};
    assert(!maton_folders_metadata("[Application]\nfilesystems=host;\n[Context]\nfilesystems=xdg-download:ro;xdg-download/reports:rw;xdg-download/space\\sname:create;xdg-documents;!xdg-download/blocked;\n",&folders));
    assert(folders.count==3 && folders.items[0].readonly);
    assert(!strcmp(folders.items[1].subpath,"reports") && !folders.items[1].readonly);
    assert(!strcmp(folders.items[2].subpath,"space name"));
    char* wire=maton_folders_encode(&folders);assert(wire);
    MatonFolderRequests decoded={0};assert(!maton_folders_decode(wire,&decoded,0));
    assert(decoded.count==folders.count);
    char* roundtrip=maton_folders_encode(&decoded);assert(!strcmp(wire,roundtrip));
    free(roundtrip);free(wire);maton_folders_clear(&decoded);maton_folders_clear(&folders);
    assert(!maton_folders_metadata("[Context]\nfilesystems=home:ro;host;xdg-download:ro;\n",&folders));
    assert(folders.count==1 && !folders.items[0].readonly);maton_folders_clear(&folders);
    assert(maton_folders_metadata("[Context]\nfilesystems=xdg-download/../other;\n",&folders)<0);
    assert(maton_folders_metadata("[Context]\nfilesystems=xdg-download/a//b;\n",&folders)<0);
    assert(maton_folders_metadata("[Context]\nfilesystems=xdg-download/a/;\n",&folders)<0);
    assert(!maton_folders_metadata("[Context]\nfilesystems=xdg-download/a\\nname;\n",&folders));
    assert(!strcmp(folders.items[0].subpath,"a\nname"));maton_folders_clear(&folders);
    char* printed=maton_permissions_unescape("[Context]\nfilesystems=xdg-download/a\\\\;b;xdg-download/c\\\\\\\\d;\n");
    assert(printed && !maton_folders_metadata(printed,&folders));free(printed);
    assert(folders.count==2 && !strcmp(folders.items[0].subpath,"a;b") && !strcmp(folders.items[1].subpath,"c\\d"));
    maton_folders_clear(&folders);
    printed=maton_permissions_unescape("\\u200B\\U0001F600\\x7F");
    assert(printed && !strcmp(printed,"\xe2\x80\x8b\xf0\x9f\x98\x80\x7f"));free(printed);
    assert(!maton_permissions_unescape("trailing\\"));
    assert(!maton_permissions_unescape("\\x0"));assert(!maton_permissions_unescape("\\x00"));
    assert(!maton_folder_subpath("/absolute"));assert(!maton_folder_subpath("."));
    assert(!maton_folder_subpath("a/../b"));assert(maton_folder_subpath("a..b/c"));
    for(unsigned i=0;i<4096;i++) {
        char name[32];snprintf(name,sizeof(name),"folder-%u",i);
        assert(!maton_folder_add(&folders,name,i%2));
    }
    wire=maton_folders_encode(&folders);assert(wire);
    assert(!maton_folders_decode(wire,&decoded,0) && decoded.count==4096);
    maton_folders_clear(&decoded);maton_folders_clear(&folders);free(wire);
    assert(maton_folders_decode("-1:0:2e2e\n",&folders,0)<0);
    assert(maton_folders_decode("-1:0:00\n",&folders,0)<0);
    assert(maton_folders_decode("-1:0:zz\n",&folders,0)<0);
    assert(maton_folders_decode("-1:0:\n",&folders,1)<0);
    maton_folders_clear(&folders);
}

static void put(int directory,const char* name,const char* contents) {
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);assert(fd>=0);
    assert(write(fd,contents,strlen(contents))==(ssize_t)strlen(contents));close(fd);
}
static void user_dirs(int root) {
    assert(!maton_download_user_dirs(root,"org.example.App"));
    int config=safe_directory_at(root,".var/app/org.example.App/config",0);assert(config>=0);
    put(config,"user-dirs.dirs","# keep\nXDG_DOCUMENTS_DIR=\"$HOME/Documents\"\nXDG_DOWNLOAD_DIR=\"/old\"\nXDG_MUSIC_DIR=\"$HOME/Music\"");
    assert(!maton_download_user_dirs(root,"org.example.App"));
    int fd=openat(config,"user-dirs.dirs",O_RDONLY|O_NOFOLLOW);assert(fd>=0);
    char text[512]={0};assert(read(fd,text,sizeof(text)-1)>0);close(fd);
    assert(!strcmp(text,"# keep\nXDG_DOCUMENTS_DIR=\"$HOME/Documents\"\nXDG_MUSIC_DIR=\"$HOME/Music\"\nXDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n"));
    assert(unlinkat(config,"user-dirs.dirs",0)==0);
    assert(symlinkat("../../../../outside",config,"user-dirs.dirs")==0);
    assert(maton_download_user_dirs(root,"org.example.App")<0);
    assert(unlinkat(config,"user-dirs.dirs",0)==0);close(config);
    assert(unlinkat(root,"Downloads",AT_REMOVEDIR)==0);
    assert(symlinkat("outside",root,"Downloads")==0);
    assert(maton_download_user_dirs(root,"org.example.App")<0);
    assert(unlinkat(root,"Downloads",0)==0);
}
static void paths(int root) {
    assert(mkdirat(root,"outside",0700)==0);
    assert(symlinkat("outside",root,"link")==0);
    assert(maton_media_directory_at(root,"link/sub",1)<0);
    assert(maton_media_directory_at(root,"outside/../escape",1)<0);
    assert(maton_media_directory_at(root,"/outside",0)<0);
    assert(mkdirat(root,"nested",0700)==0);
    assert(mkdirat(root,"nested/sub",0700)==0);
    int child=maton_media_directory_at(root,"nested/sub",0);assert(child>=200);
    assert(fcntl(child,F_GETFL)&O_PATH);struct stat st;assert(!fstat(child,&st) && S_ISDIR(st.st_mode));close(child);
    put(root,"file","unchanged");
    assert(maton_media_directory_at(root,"file/sub",1)<0);
    assert(unlinkat(root,"file",0)==0);assert(unlinkat(root,"link",0)==0);
}
int main(void) {
    metadata();
    char path[]="/tmp/maton-downloads-XXXXXX";assert(mkdtemp(path));
    int root=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);assert(root>=0);
    paths(root);user_dirs(root);
    assert(unlinkat(root,"nested/sub",AT_REMOVEDIR)==0);
    assert(unlinkat(root,"nested",AT_REMOVEDIR)==0);
    assert(unlinkat(root,"outside",AT_REMOVEDIR)==0);
    assert(unlinkat(root,".var/app/org.example.App/config",AT_REMOVEDIR)==0);
    assert(unlinkat(root,".var/app/org.example.App",AT_REMOVEDIR)==0);
    assert(unlinkat(root,".var/app",AT_REMOVEDIR)==0);assert(unlinkat(root,".var",AT_REMOVEDIR)==0);
    close(root);assert(rmdir(path)==0);puts("Downloads parsing, wire, path and user-dirs tests passed");
    return 0;
}
