#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_DIM 8192u
#define MAX_PIXELS (MAX_DIM * MAX_DIM)
#define MAX_ZIP_ENTRIES 8
#define FRAME_COUNT 4
#define FPS 8

static const char *prog = "matonos-efi-logo2";
static void logmsg(const char *level, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: %s: ", prog, level);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
static void logerr(const char *what, const char *path) {
    int e = errno;
    logmsg("ERROR", "%s path=%s errno=%d (%s)", what, path, e, strerror(e));
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, (uint16_t)v); put16(f, (uint16_t)(v >> 16)); }
static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}
static uint32_t crc32(const uint8_t *p, size_t n) { return crc_update(0, p, n); }
static int read_file(const char *path, uint8_t **out, size_t *len, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { logerr("open failed", path); return -1; }
    if (fseek(f, 0, SEEK_END) || (long)(*len = (size_t)ftell(f)) < 0 || *len > cap || fseek(f, 0, SEEK_SET)) {
        int e = errno ? errno : EFBIG; fclose(f); errno = e; logerr("size/seek failed", path); return -1;
    }
    *out = malloc(*len ? *len : 1);
    if (!*out) { fclose(f); errno = ENOMEM; logerr("allocation failed", path); return -1; }
    if (fread(*out, 1, *len, f) != *len) { int e = ferror(f) ? errno : EIO; free(*out); *out = NULL; fclose(f); errno = e; logerr("read failed", path); return -1; }
    if (fclose(f)) { logerr("close failed", path); free(*out); *out = NULL; return -1; }
    return 0;
}
static int read_text(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) { logerr("open failed", path); return -1; }
    if (!fgets(buf, (int)cap, f)) { int e = ferror(f) ? errno : EIO; fclose(f); errno = e; logerr("read failed", path); return -1; }
    if (fclose(f)) { logerr("close failed", path); return -1; }
    return 0;
}
static int join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; logerr("path too long", a); return -1; }
    return 0;
}
static int parse_uint(const char *s, unsigned *v) {
    char *end; errno = 0; unsigned long x = strtoul(s, &end, 0);
    if (errno || end == s || (*end && *end != '\n') || x > UINT_MAX) { errno = EINVAL; return -1; }
    *v = (unsigned)x; return 0;
}

struct image { unsigned w, h; uint8_t *rgb; };
static int bmp_read(const char *path, struct image *im) {
    uint8_t *b = NULL; size_t n = 0;
    if (read_file(path, &b, &n, 128u * 1024u * 1024u)) return -1;
    if (n < 54 || b[0] != 'B' || b[1] != 'M') { errno = EINVAL; logerr("invalid BMP header", path); free(b); return -1; }
    uint32_t off = le32(b + 10), dib = le32(b + 14);
    int32_t wi = (int32_t)le32(b + 18), hi = (int32_t)le32(b + 22);
    uint16_t planes = le16(b + 26), bits = le16(b + 28); uint32_t comp = le32(b + 30);
    if (dib < 40 || wi <= 0 || hi == 0 || hi == INT32_MIN || planes != 1 || (bits != 24 && bits != 32) || comp != 0) {
        errno = EINVAL; logerr("unsupported BMP (need BI_RGB 24/32-bit)", path); free(b); return -1;
    }
    unsigned w = (unsigned)wi, h = (unsigned)(hi < 0 ? -hi : hi);
    uint64_t row = ((uint64_t)w * bits / 8 + 3) & ~3ull;
    if (w > MAX_DIM || h > MAX_DIM || (uint64_t)w * h > MAX_PIXELS || off > n || row * h > n - off) {
        errno = EFBIG; logerr("BMP dimensions/data out of bounds", path); free(b); return -1;
    }
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) { errno = ENOMEM; logerr("BMP allocation failed", path); free(b); return -1; }
    for (unsigned y = 0; y < h; y++) {
        unsigned sy = hi > 0 ? h - 1 - y : y;
        const uint8_t *src = b + off + (size_t)sy * row;
        for (unsigned x = 0; x < w; x++) {
            rgb[((size_t)y * w + x) * 3 + 0] = src[x * (bits / 8) + 2];
            rgb[((size_t)y * w + x) * 3 + 1] = src[x * (bits / 8) + 1];
            rgb[((size_t)y * w + x) * 3 + 2] = src[x * (bits / 8) + 0];
        }
    }
    free(b); im->w = w; im->h = h; im->rgb = rgb; return 0;
}
struct rgba { unsigned w,h; uint8_t *p; };
static int load_wordmark(const char *path, struct rgba *wm) {
    uint8_t *raw = NULL; size_t n = 0;
    if (read_file(path, &raw, &n, 8u * 1024u * 1024u)) return -1;
    if (n < 8 || memcmp(raw, "MRGBA1\0", 7)) { errno = EINVAL; logerr("invalid raw wordmark asset", path); free(raw); return -1; }
    if (n < 16) { errno = EINVAL; logerr("truncated raw wordmark asset", path); free(raw); return -1; }
    wm->w = le32(raw + 8); wm->h = le32(raw + 12);
    if (wm->w == 0 || wm->h == 0 || wm->w > 4096 || wm->h > 1024 || n != 16u + (size_t)wm->w * wm->h * 4) {
        errno = EINVAL; logerr("bad raw wordmark dimensions", path); free(raw); return -1;
    }
    wm->p = malloc(n - 16);
    if (!wm->p) { errno = ENOMEM; logerr("wordmark allocation failed", path); free(raw); return -1; }
    memcpy(wm->p, raw + 16, n - 16); free(raw); return 0;
}
static void blend(uint8_t *dst, const uint8_t *src, unsigned a) {
    for (int c = 0; c < 3; c++) dst[c] = (uint8_t)((dst[c] * (255 - a) + src[c] * a + 127) / 255);
}
static int overlay_bgrt(struct image *frame, const struct image *logo, unsigned ox, unsigned oy) {
    unsigned x0 = ox < frame->w ? ox : frame->w, y0 = oy < frame->h ? oy : frame->h;
    unsigned cw = logo->w < frame->w - x0 ? logo->w : frame->w - x0;
    unsigned ch = logo->h < frame->h - y0 ? logo->h : frame->h - y0;
    for (unsigned y = 0; y < ch; y++) memcpy(frame->rgb + ((size_t)(y0+y)*frame->w+x0)*3, logo->rgb + (size_t)y*logo->w*3, (size_t)cw*3);
    return 0;
}
static void overlay_wordmark(struct image *frame, const struct rgba *wm, int phase, unsigned logo_y, unsigned logo_h) {
    unsigned maxw = frame->w * 3 / 5, maxh = frame->h / 8;
    if (maxw < 1 || maxh < 1) return;
    unsigned w = wm->w < maxw ? wm->w : maxw;
    unsigned h = (unsigned)((uint64_t)wm->h * w / wm->w);
    if (h > maxh) { h = maxh; w = (unsigned)((uint64_t)wm->w * h / wm->h); }
    if (!w || !h) return;
    int x = (int)(frame->w - w) / 2;
    unsigned y = frame->h * 2 / 3;
    if (y < logo_y + logo_h + 24) y = logo_y + logo_h + 24;
    if (y + h + 12 >= frame->h) y = frame->h > h + 12 ? frame->h - h - 12 : 0;
    for (unsigned yy = 0; yy < h; yy++) for (unsigned xx = 0; xx < w; xx++) {
        unsigned sx = (unsigned)((uint64_t)xx * wm->w / w), sy = (unsigned)((uint64_t)yy * wm->h / h);
        const uint8_t *s = wm->p + ((size_t)sy * wm->w + sx) * 4;
        uint8_t *d = frame->rgb + ((size_t)(y + yy) * frame->w + (unsigned)x + xx) * 3;
        blend(d, s, s[3]);
    }
    unsigned barw = frame->w / 10; if (barw < 24) barw = frame->w < 24 ? frame->w : 24;
    unsigned bh = frame->h >= 400 ? 6 : 3;
    unsigned bx = (frame->w - barw) / 2, by = y + h + 8;
    if (by + bh >= frame->h) by = frame->h - bh - 1;
    for (unsigned yy = 0; yy < bh; yy++) for (unsigned xx = 0; xx < barw; xx++) {
        uint8_t *d = frame->rgb + ((size_t)(by + yy) * frame->w + bx + xx) * 3;
        d[0] = d[1] = d[2] = 40;
    }
    unsigned seg = barw / 5, start = (unsigned)(phase % 5) * (barw - seg) / 4;
    for (unsigned yy = 0; yy < bh; yy++) for (unsigned xx = 0; xx < seg; xx++) {
        uint8_t *d = frame->rgb + ((size_t)(by + yy) * frame->w + bx + start + xx) * 3;
        d[0] = d[1] = d[2] = 230;
    }
}
static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return b << 16 | a;
}
static void mem_put32be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static void png_chunk(uint8_t *buf, size_t *pos, const char type[4], const uint8_t *p, uint32_t n) {
    mem_put32be(buf + *pos, n); *pos += 4;
    memcpy(buf + *pos, type, 4); *pos += 4;
    if (n) { memcpy(buf + *pos, p, n); *pos += n; }
    uint32_t c = crc_update(crc32((const uint8_t *)type,4), p, n);
    mem_put32be(buf + *pos, c); *pos += 4;
}
static int make_png(const struct image *im, uint8_t **out, size_t *outn) {
    size_t rowbytes = (size_t)im->w * 3 + 1, rawlen = rowbytes * im->h;
    uint8_t *raw = malloc(rawlen); if (!raw) { errno=ENOMEM; return -1; }
    for (unsigned y=0;y<im->h;y++){ raw[(size_t)y*rowbytes]=0; memcpy(raw+(size_t)y*rowbytes+1, im->rgb+(size_t)y*im->w*3, (size_t)im->w*3); }
    size_t blocks=(rawlen+65534)/65535, zlen=2+rawlen+blocks*5+4;
    uint8_t *z=malloc(zlen); if(!z){free(raw);errno=ENOMEM;return -1;}
    size_t at=0, pos=0; z[pos++]=0x78; z[pos++]=0x01;
    while(at<rawlen){ size_t n=rawlen-at; if(n>65535)n=65535; z[pos++]=(at+n==rawlen)?1:0; z[pos++]=(uint8_t)n; z[pos++]=(uint8_t)(n>>8); uint16_t inv=(uint16_t)~n; z[pos++]=(uint8_t)inv; z[pos++]=(uint8_t)(inv>>8); memcpy(z+pos,raw+at,n); pos+=n; at+=n; }
    uint32_t ad=adler32(raw,rawlen); z[pos++]=ad>>24; z[pos++]=ad>>16; z[pos++]=ad>>8; z[pos++]=ad;
    free(raw);
    size_t total=8+25+12+zlen+12;
    if(zlen>UINT32_MAX){free(z);errno=EFBIG;return -1;}
    uint8_t *png=malloc(total); if(!png){free(z);errno=ENOMEM;return -1;}
    static const uint8_t sig[]={137,80,78,71,13,10,26,10}; memcpy(png,sig,8); pos=8;
    uint8_t ihdr[13]={(uint8_t)(im->w>>24),(uint8_t)(im->w>>16),(uint8_t)(im->w>>8),(uint8_t)im->w,(uint8_t)(im->h>>24),(uint8_t)(im->h>>16),(uint8_t)(im->h>>8),(uint8_t)im->h,8,2,0,0,0};
    png_chunk(png,&pos,"IHDR",ihdr,13); png_chunk(png,&pos,"IDAT",z,(uint32_t)zlen); png_chunk(png,&pos,"IEND",NULL,0); free(z);
    if(pos!=total){free(png);errno=EIO;return -1;} *out=png; *outn=total; return 0;
}
struct zentry { char name[32]; uint32_t crc,size,offset; };
static int zip_entry(FILE *f, struct zentry *e, const char *name, const uint8_t *data, size_t n) {
    if(n>UINT32_MAX||strlen(name)>31||ftell(f)<0||(unsigned long)ftell(f)>UINT32_MAX){errno=EFBIG;return -1;}
    e->offset=(uint32_t)ftell(f); e->size=(uint32_t)n; e->crc=crc32(data,n); strcpy(e->name,name);
    put32(f,0x04034b50); put16(f,20); put16(f,0); put16(f,0); put16(f,0); put16(f,0); put32(f,e->crc); put32(f,e->size); put32(f,e->size); put16(f,(uint16_t)strlen(name)); put16(f,0); fwrite(name,1,strlen(name),f); fwrite(data,1,n,f);
    return ferror(f)?-1:0;
}
static int zip_finish(FILE *f, struct zentry *e, unsigned count) {
    long cd=ftell(f); if(cd<0||cd>UINT32_MAX)return -1;
    for(unsigned i=0;i<count;i++){uint16_t nl=(uint16_t)strlen(e[i].name);put32(f,0x02014b50);put16(f,20);put16(f,20);put16(f,0);put16(f,0);put16(f,0);put16(f,0);put32(f,e[i].crc);put32(f,e[i].size);put32(f,e[i].size);put16(f,nl);put16(f,0);put16(f,0);put16(f,0);put16(f,0);put32(f,0);put32(f,e[i].offset);fwrite(e[i].name,1,nl,f);}
    long end=ftell(f); if(end<0||end>UINT32_MAX||end-cd>UINT32_MAX)return -1;
    put32(f,0x06054b50);put16(f,0);put16(f,0);put16(f,count);put16(f,count);put32(f,(uint32_t)(end-cd));put32(f,(uint32_t)cd);put16(f,0);
    return ferror(f)?-1:0;
}
static int read_mode(const char *root, unsigned *w, unsigned *h) {
    char dir[PATH_MAX]; if(join(dir,sizeof(dir),root,"sys/class/drm"))return -1;
    DIR *d=opendir(dir); if(!d){logerr("DRM sysfs unavailable",dir);return -1;}
    struct dirent *de; int found=0;
    while((de=readdir(d))){if(strncmp(de->d_name,"card",4)||!strchr(de->d_name,'-'))continue;
        char p[PATH_MAX], base[PATH_MAX], text[128]; if(join(base,sizeof(base),dir,de->d_name)||join(p,sizeof(p),base,"status"))continue;
        if (read_text(p,text,sizeof(text))) continue;
        if (strncmp(text,"connected",9)) continue;
        if(join(p,sizeof(p),base,"modes")||read_text(p,text,sizeof(text)))continue;
        unsigned mw,mh; if(sscanf(text,"%ux%u",&mw,&mh)==2&&mw&&mh&&mw>=64&&mh>=64&&mw<=MAX_DIM&&mh<=MAX_DIM){*w=mw;*h=mh;found=1;break;}
    }
    if(closedir(d)){logerr("close DRM directory failed",dir);}
    if(!found){errno=ENODEV;logerr("no connected DRM connector with a readable mode",dir);return -1;} return 0;
}
static int write_zip(const char *outpath, const char *root, const char *wordmark) {
    char p[PATH_MAX], text[128]; unsigned status,x,y,w,h;
    if(join(p,sizeof(p),root,"sys/firmware/acpi/bgrt/status")) return -1;
    FILE *sf=fopen(p,"r");
    if(!sf){if(errno==ENOENT||errno==ENOTDIR){logmsg("INFO","BGRT status missing; keeping static fallback");return 1;}logerr("BGRT status open failed",p);return -1;}
    if(!fgets(text,sizeof(text),sf)){int e=ferror(sf)?errno:EIO;fclose(sf);errno=e;logerr("BGRT status read failed",p);return -1;}
    if(fclose(sf)){logerr("BGRT status close failed",p);return -1;}
    if(parse_uint(text,&status)){logmsg("INFO","BGRT status invalid; keeping static fallback");return 1;}
    if(!(status&1)){logmsg("INFO","BGRT status=0x%x image-valid bit clear; keeping static fallback",status);return 1;}
    if(join(p,sizeof(p),root,"sys/firmware/acpi/bgrt/xoffset")||read_text(p,text,sizeof(text))||parse_uint(text,&x)||join(p,sizeof(p),root,"sys/firmware/acpi/bgrt/yoffset")||read_text(p,text,sizeof(text))||parse_uint(text,&y)){logmsg("ERROR","BGRT offsets unavailable root=%s errno=%d (%s)",root,errno,strerror(errno));return -1;}
    struct image logo={0}; struct rgba wm={0};
    if(join(p,sizeof(p),root,"sys/firmware/acpi/bgrt/image")||bmp_read(p,&logo)||read_mode(root,&w,&h)||load_wordmark(wordmark,&wm))goto fail;
    if((uint64_t)w*h>MAX_PIXELS || (uint64_t)w*h*3*FRAME_COUNT>256u*1024u*1024u){errno=EFBIG;logmsg("ERROR","panel animation exceeds 256 MiB output bound (%ux%u)",w,h);goto fail;}
    struct image base={.w=w,.h=h,.rgb=calloc((size_t)w*h,3)}; if(!base.rgb){errno=ENOMEM;goto fail;}
    char tmppath[PATH_MAX];
    int tlen=snprintf(tmppath,sizeof(tmppath),"%s.tmp",outpath);
    if(tlen<0||(size_t)tlen>=sizeof(tmppath)){errno=ENAMETOOLONG;logerr("temporary output path too long",outpath);free(base.rgb);goto fail;}
    struct zentry entries[MAX_ZIP_ENTRIES]; unsigned count=0; FILE *zf=fopen(tmppath,"wb"); if(!zf){logerr("temporary output open failed",tmppath);free(base.rgb);goto fail;}
    char desc[96]; int dl=snprintf(desc,sizeof(desc),"%u %u %u\np 0 0 part0\n",w,h,FPS);
    if(zip_entry(zf,&entries[count++],"desc.txt",(uint8_t*)desc,(size_t)dl)){logerr("ZIP desc write failed",tmppath);goto zipfail;}
    for(int i=0;i<FRAME_COUNT;i++){
        memset(base.rgb,0,(size_t)w*h*3);
        overlay_bgrt(&base,&logo,x,y); overlay_wordmark(&base,&wm,i,y,logo.h);
        uint8_t *png=NULL;size_t pngn=0;if(make_png(&base,&png,&pngn)){logerr("PNG encode failed",tmppath);goto zipfail;}
        char name[32];snprintf(name,sizeof(name),"part0/%04d.png",i);
        int rc=zip_entry(zf,&entries[count++],name,png,pngn);free(png);if(rc){logerr("ZIP frame write failed",tmppath);goto zipfail;}
    }
    if(zip_finish(zf,entries,count)||fflush(zf)){logerr("ZIP finalize failed",tmppath);goto zipfail;}
    if(fclose(zf)){logerr("ZIP close failed",tmppath);if(unlink(tmppath))logerr("temporary ZIP removal failed",tmppath);goto fail;}
    if(chmod(tmppath,0644)){logerr("temporary ZIP chmod failed; fallback preserved",tmppath);if(unlink(tmppath))logerr("temporary ZIP removal failed",tmppath);goto fail;}
    if(rename(tmppath,outpath)){logerr("atomic ZIP replace failed; fallback preserved",outpath);if(unlink(tmppath))logerr("temporary ZIP removal failed",tmppath);goto fail;}
    free(base.rgb);free(logo.rgb);free(wm.p);logmsg("INFO","composed %ux%u frames with BGRT at %u,%u output=%s",w,h,x,y,outpath);return 0;
zipfail: {int e=errno;if(fclose(zf))logerr("ZIP close failed after write error",tmppath);if(unlink(tmppath))logerr("partial temporary ZIP removal failed",tmppath);errno=e;logerr("ZIP output failed; fallback preserved",tmppath);free(base.rgb);}
fail: if(errno)logmsg("ERROR","composition failed; static fallback remains root=%s errno=%d (%s)",root,errno,strerror(errno));free(logo.rgb);free(wm.p);return -1;
}
int main(int argc,char **argv){
    const char *root="", *out="/oem/media/bootanimation.zip", *wm="/odm/etc/bootanim/wordmark.rgba";
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--root")&&i+1<argc)root=argv[++i];else if(!strcmp(argv[i],"--output")&&i+1<argc)out=argv[++i];else if(!strcmp(argv[i],"--wordmark")&&i+1<argc)wm=argv[++i];else{logmsg("ERROR","usage: %s [--root SYSFS_ROOT] [--output ZIP] [--wordmark RGBA]",prog);return 2;}}
    int rc=write_zip(out,root,wm);return rc==0?0:rc==1?0:1;
}
