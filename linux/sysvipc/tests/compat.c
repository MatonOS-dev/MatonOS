/* SPDX-License-Identifier: Apache-2.0 */
/* Real ELF32 workload, no libc/multilib dependency. Layouts are explicit UAPI
 * bytes rather than native host structures. */
typedef unsigned int u32;
typedef unsigned short u16;
extern int call6(int,u32,u32,u32,u32,u32,u32);
#define CALL(n,a,b,c,d,e,f) call6(n,(u32)(a),(u32)(b),(u32)(c),(u32)(d),(u32)(e),(u32)(f))
#define CHECK(x) do{if(!(x))return __LINE__;}while(0)
struct op {u16 n;short value,flags;};
static u32 get32(unsigned char *b,int p){return *(u32 *)(b+p);}
static u16 get16(unsigned char *b,int p){return *(u16 *)(b+p);}
static void put16(unsigned char *b,int p,u16 n){*(u16 *)(b+p)=n;}
int compat_main(void){
    unsigned char ds[128]={0};
    int id=CALL(393,0x4d410001,2,0600|01000|02000,0,0,0);CHECK(id>=0);
    CHECK(CALL(394,id,0,16,1,0,0)==0);
    CHECK(CALL(394,id,0,2,ds,0,0)==0);CHECK(get32(ds,52)==2&&get32(ds,20)==0600);
    struct op ops[2]={{0,-1,04000},{1,-1,04000}};
    CHECK(CALL(117,1,id,2,0,ops,0)==-11);CHECK(CALL(394,id,0,12,0,0,0)==1);
    u32 arg=0;CHECK(CALL(117,3,id,0,12,&arg,0)==1);
    arg=(u32)ds;CHECK(CALL(117,3,id,0,2|0x100,&arg,0)==0);CHECK(get32(ds,52)==2);
    CHECK(CALL(117,3,id,0,2,&arg,0)==0);CHECK(get16(ds,40)==2&&get16(ds,12)==0600);
    put16(ds,12,0400);CHECK(CALL(117,3,id,0,1,&arg,0)==0);
    CHECK(CALL(394,id,0,16,0,0,0)==-13);
    put16(ds,12,0600);CHECK(CALL(117,3,id,0,1,&arg,0)==0);
    arg=0;CHECK(CALL(117,3,id,0,16,&arg,0)==0);
    int time32[2]={0,1000000};
    CHECK(CALL(117,4,id,1,0,ops,time32)==-11);
    long long time64[2]={0,1000000};CHECK(CALL(420,id,ops,1,time64,0,0)==-11);
    CHECK(CALL(394,id,0,0,0,0,0)==0);
    id=CALL(117,2,0,1,0600,0,0);CHECK(id>=0);arg=0;CHECK(CALL(117,3,id,0,0,&arg,0)==0);
    id=CALL(399,0x4d410003,0600|01000|02000,0,0,0,0);CHECK(id>=0);
    struct {int type;char data[8];} msg={7,"compat"};
    CHECK(CALL(400,id,&msg,7,0,0,0)==0);msg.type=2;
    CHECK(CALL(117,11,id,7,0,&msg,0)==0);
    CHECK(CALL(402,id,2,ds,0,0,0)==0);CHECK(get32(ds,64)==2&&get32(ds,68)==16384);
    struct {u32 ptr;int type;} kludge={(u32)&msg,-7};
    CHECK(CALL(117,12,id,8,0,&kludge,0)==7&&msg.type==2);
    CHECK(CALL(401,id,&msg,8,7,0,0)==7&&msg.type==7);
    CHECK(CALL(117,14,id,2,0,ds,0)==0);CHECK(get16(ds,46)==0);
    CHECK(CALL(117,14,id,0,0,0,0)==0);
    id=CALL(395,0x4d410002,123,0600|01000|02000,0,0,0);CHECK(id>=0);
    CHECK(CALL(396,id,2,ds,0,0,0)==0);CHECK(get32(ds,36)==123&&get32(ds,72)==0);
    /* A freestanding (static) program has no shmat helper: like a kernel
     * without SysV shm, shmat is ENOSYS; the segment itself is unaffected. */
    CHECK(CALL(397,id,0,0,0,0,0)==-38);
    CHECK(CALL(117,21,id,0,0,0,0)==-38);
    CHECK(CALL(396,id,2,ds,0,0,0)==0);CHECK(get32(ds,72)==0);
    CHECK(CALL(396,id,0,0,0,0,0)==0);
    CHECK(CALL(396,id,2,ds,0,0,0)==-22);
    id=CALL(117,23,0,4096,0600,0,0);CHECK(id>=0);CHECK(CALL(117,24,id,0,0,0,0)==0);
    /* Explicit IPC_INFO/INFO layouts on both old and IPC_64 interfaces. */
    CHECK(CALL(396,0,3,ds,0,0,0)>=0);CHECK(get32(ds,4)==1&&get32(ds,8)==4096);
    CHECK(CALL(117,24,0,3,0,ds,0)>=0);CHECK(get32(ds,0)==0x7fffffffu);
    const char out[]="PASS: ELF32 direct syscalls, ipc mux, old/IPC_64 structs, time32/time64, helperless shmat ENOSYS\n";
    CHECK(CALL(4,1,out,sizeof(out)-1,0,0,0)==sizeof(out)-1);return 0;
}
