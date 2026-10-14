#define MATONOS_PUBLISH_HOST_TEST
#include "../FlatpakPublish.c"
#include <assert.h>
static unsigned count;
static void progress(const char* line) {
    assert(!strcmp(line,"first")||!strcmp(line,"second")||!strcmp(line,"tail"));
    count++;
}
int main(void) {
    char* args[]={"sh","-c","printf fir; printf 'st\\nsecond\\rtail'",NULL};
    char output[8]={0};
    flatpak_publish_progress(progress);
    assert(!invoke_path_capture("/bin/sh",NULL,NULL,args,output,sizeof(output),NULL));
    assert(count==3); /* includes final unterminated line and truncated capture */
    flatpak_publish_progress(NULL);
    assert(!invoke_path_capture("/bin/sh",NULL,NULL,args,output,sizeof(output),NULL));
    assert(count==3);
    puts("PASS staging output observer, partial lines, CR, trailing line and bounded capture");
}
