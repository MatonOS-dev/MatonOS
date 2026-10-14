#ifndef MATON_STORE_UPDATE_H
#define MATON_STORE_UPDATE_H
#include <string.h>
#include <stddef.h>
/* Accept one complete SHA-256 line only: warnings/truncation are not commits. */
static inline int store_parse_commit(const char* text,char* out,size_t size) {
    if(!text || size<65)return -1;
    size_t n=strlen(text);
    if(n==65 && text[64]=='\n')n--;
    else if(n==66 && text[64]=='\r' && text[65]=='\n')n-=2;
    if(n!=64)return -1;
    for(size_t i=0;i<64;i++)if(!((text[i]>='0'&&text[i]<='9') ||
            (text[i]>='a'&&text[i]<='f')))return -1;
    memcpy(out,text,64);out[64]=0;return 0;
}
#endif
