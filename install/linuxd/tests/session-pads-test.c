/* Host-only: exercise FF handshake/state and event validation without uinput. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
static int fake_ioctl(int fd, unsigned long request, ...);
#define ioctl fake_ioctl
#include "../SessionPads.c"
#undef ioctl
int maton_udev_refresh(void){return 0;}
void maton_udev_forget_pad(dev_t device){(void)device;}
static struct ff_effect upload;
static unsigned erase;
static int result, begins, ends;
static int fake_ioctl(int fd,unsigned long request,...) {
    (void)fd;va_list ap;va_start(ap,request);void *arg=va_arg(ap,void*);va_end(ap);
    if(request==UI_BEGIN_FF_UPLOAD){struct uinput_ff_upload *u=arg;u->effect=upload;begins++;}
    else if(request==UI_END_FF_UPLOAD){result=((struct uinput_ff_upload*)arg)->retval;ends++;}
    else if(request==UI_BEGIN_FF_ERASE){((struct uinput_ff_erase*)arg)->effect_id=erase;begins++;}
    else if(request==UI_END_FF_ERASE){result=((struct uinput_ff_erase*)arg)->retval;ends++;}
    return 0;
}
static unsigned calls;static uint16_t strong,weak;
static void rumble(void *arg,unsigned slot,uint16_t a,uint16_t b){(void)arg;assert(slot==0);calls++;strong=a;weak=b;}
int main(void) {
    assert(!maton_pads_create(NULL,5,10000,NULL,NULL));
    assert(!maton_pads_create(NULL,0,1000,NULL,NULL));
    MatonSessionPads *empty=maton_pads_create(NULL,0,10000,NULL,NULL);
    assert(empty && !*maton_pads_nodes(empty));maton_pads_destroy(empty);
    MatonSessionPads s={.count=1,.rumble=rumble};s.pad[0].playing=-1;
    upload=(struct ff_effect){.id=2,.type=FF_RUMBLE,.replay={.length=50},.u.rumble={123,456}};
    struct input_event e={.type=EV_UINPUT,.code=UI_FF_UPLOAD,.value=42};
    feedback(&s,0,&e);assert(result==0 && s.pad[0].valid[2] && begins==ends);
    e=(struct input_event){.type=EV_FF,.code=2,.value=1};feedback(&s,0,&e);
    assert(calls==1 && strong==123 && weak==456 && s.pad[0].stop_at>milliseconds());
    erase=2;e=(struct input_event){.type=EV_UINPUT,.code=UI_FF_ERASE,.value=7};feedback(&s,0,&e);
    assert(result==0 && !s.pad[0].valid[2] && strong==0 && weak==0 && begins==ends);
    upload.type=FF_PERIODIC;e.code=UI_FF_UPLOAD;feedback(&s,0,&e);assert(result==-ENOSYS);
    upload.type=FF_RUMBLE;upload.id=EFFECTS;feedback(&s,0,&e);assert(result==-ENOSYS);
    upload.id=1;s.rumble=NULL;feedback(&s,0,&e);assert(result==-ENOSYS && begins==ends);
    int pipefd[2];assert(!pipe(pipefd));s.pad[0].fd=pipefd[1];
    for(unsigned a=0;a<MATON_PAD_AXES;a++)s.pad[0].abs[a]=(struct input_absinfo){.minimum=a<4?-32768:a>=6?-1:0,.maximum=a<4?32767:a>=6?1:1023};
    e=(struct input_event){.type=EV_KEY,.code=KEY_A,.value=1};assert(maton_pads_send(&s,0,&e,1)==-1);
    e=(struct input_event){.type=EV_ABS,.code=ABS_Z,.value=1024};assert(maton_pads_send(&s,0,&e,1)==-1);
    assert(maton_pads_send(&s,4,&e,1)==-1);
    assert(!maton_pads_release(&s,0));struct input_event batch[32];
    ssize_t n=read(pipefd[0],batch,sizeof(batch));assert(n==26*(ssize_t)sizeof(e));
    assert(batch[25].type==EV_SYN && batch[25].code==SYN_REPORT);
    close(pipefd[0]);close(pipefd[1]);puts("PASS: empty session, bounds, FF upload/erase/play/cancel, no-vibrator rejection, release events");
}
