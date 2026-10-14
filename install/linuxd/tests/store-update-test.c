#include "../StoreUpdate.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    char output[65];
    const char* commit="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(!store_parse_commit(commit,output,sizeof(output)) && !strcmp(output,commit));
    char text[256];snprintf(text,sizeof(text),"%s\n",commit);assert(!store_parse_commit(text,output,sizeof(output)));
    snprintf(text,sizeof(text),"%s\r\n",commit);assert(!store_parse_commit(text,output,sizeof(output)));
    const char* bad[]={"", "abc", "warning\n", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefjunk",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg", "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(store_parse_commit(bad[i],output,sizeof(output)));
    assert(store_parse_commit(NULL,output,sizeof(output)));
    assert(store_parse_commit(commit,output,64));
    puts("PASS strict update commit parser");
}
