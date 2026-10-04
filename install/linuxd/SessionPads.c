#define _GNU_SOURCE
#include "SessionPads.h"
#include "UdevDatabase.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EFFECTS 16
static const unsigned keys[] = {BTN_SOUTH,BTN_EAST,BTN_NORTH,BTN_WEST,
    BTN_TL,BTN_TR,BTN_TL2,BTN_TR2,BTN_SELECT,BTN_START,BTN_MODE,BTN_THUMBL,
    BTN_THUMBR,BTN_DPAD_UP,BTN_DPAD_DOWN,BTN_DPAD_LEFT,BTN_DPAD_RIGHT};
static const unsigned axes[] = {ABS_X,ABS_Y,ABS_RX,ABS_RY,ABS_Z,ABS_RZ,ABS_HAT0X,ABS_HAT0Y};
typedef struct {
    int fd, created;
    dev_t device;
    char node[64];
    struct input_absinfo abs[MATON_PAD_AXES];
    struct ff_effect effects[EFFECTS];
    int valid[EFFECTS], playing;
    long long stop_at;
} Pad;
struct MatonSessionPads {
    size_t count;
    Pad pad[MATON_PAD_LIMIT];
    char nodes[MATON_PAD_LIMIT * 65];
    int stop[2], worker;
    pthread_t thread;
    pthread_mutex_t feedback_mutex;
    MatonPadRumble rumble;
    void *context;
};
static long long milliseconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void cancel(MatonSessionPads *s,unsigned slot) {
    Pad *p=&s->pad[slot];
    if(p->playing>=0 && s->rumble)s->rumble(s->context,slot,0,0);
    p->playing=-1;p->stop_at=0;
}
static void feedback(MatonSessionPads *s,unsigned slot,const struct input_event *e) {
    Pad *p=&s->pad[slot];
    if(e->type==EV_UINPUT && e->code==UI_FF_UPLOAD) {
        struct uinput_ff_upload u={.request_id=(unsigned)e->value};
        if(ioctl(p->fd,UI_BEGIN_FF_UPLOAD,&u))return;
        int id=u.effect.id;
        u.retval=-ENOSYS;
        if(id>=0 && id<EFFECTS && u.effect.type==FF_RUMBLE &&
                !u.effect.replay.delay && s->rumble) {
            p->effects[id]=u.effect;p->valid[id]=1;u.retval=0;
        }
        ioctl(p->fd,UI_END_FF_UPLOAD,&u);
    } else if(e->type==EV_UINPUT && e->code==UI_FF_ERASE) {
        struct uinput_ff_erase u={.request_id=(unsigned)e->value};
        if(ioctl(p->fd,UI_BEGIN_FF_ERASE,&u))return;
        u.retval=0;
        if(u.effect_id<EFFECTS) {
            if(p->playing==(int)u.effect_id)cancel(s,slot);
            p->valid[u.effect_id]=0;
        } else u.retval=-EINVAL;
        ioctl(p->fd,UI_END_FF_ERASE,&u);
    } else if(e->type==EV_FF && e->code<EFFECTS && p->valid[e->code]) {
        if(e->value<=0) {if(p->playing==e->code)cancel(s,slot);return;}
        struct ff_effect *f=&p->effects[e->code];
        cancel(s,slot);p->playing=e->code;
        p->stop_at=f->replay.length?milliseconds()+(long long)f->replay.length*e->value:0;
        s->rumble(s->context,slot,f->u.rumble.strong_magnitude,f->u.rumble.weak_magnitude);
    }
}
static void *watch(void *arg) {
    MatonSessionPads *s=arg;
    struct pollfd fds[MATON_PAD_LIMIT+1]={{.fd=s->stop[0],.events=POLLIN}};
    for(size_t i=0;i<s->count;i++)fds[i+1]=(struct pollfd){.fd=s->pad[i].fd,.events=POLLIN};
    for(;;) {
        int n=poll(fds,s->count+1,10);
        if(n<0 && errno==EINTR)continue;
        if(n<0 || fds[0].revents)break;
        for(unsigned i=0;i<s->count;i++) {
            if(fds[i+1].revents&POLLIN) {
                struct input_event e;
                /* Bound each drain so teardown cannot be starved by an app. */
                for(int j=0;j<64 && read(s->pad[i].fd,&e,sizeof(e))==sizeof(e);j++) {
                    pthread_mutex_lock(&s->feedback_mutex);feedback(s,i,&e);pthread_mutex_unlock(&s->feedback_mutex);
                }
            }
            pthread_mutex_lock(&s->feedback_mutex);
            if(s->pad[i].stop_at && milliseconds()>=s->pad[i].stop_at)cancel(s,i);
            pthread_mutex_unlock(&s->feedback_mutex);
        }
    }
    for(unsigned i=0;i<s->count;i++)cancel(s,i);
    return NULL;
}
static int node_for(Pad *p) {
    char name[128]={0},path[256];
    if(ioctl(p->fd,UI_GET_SYSNAME(sizeof(name)),name))return -1;
    if(strncmp(name,"input",5) || strspn(name+5,"0123456789")!=strlen(name+5)) {errno=EINVAL;return -1;}
    snprintf(path,sizeof(path),"/sys/class/input");
    for(int retry=0;retry<100;retry++) {
        DIR *d=opendir(path);struct dirent *e;
        if(d) {
            while((e=readdir(d))) {
                if(strncmp(e->d_name,"event",5) || !e->d_name[5] ||
                        strspn(e->d_name+5,"0123456789")!=strlen(e->d_name+5))continue;
                if(strlen(e->d_name)>32)continue;
                char link[512],resolved[4096];
                snprintf(link,sizeof(link),"%s/%.32s",path,e->d_name);
                if(!realpath(link,resolved))continue;
                char *last=strrchr(resolved,'/');if(!last)continue;*last=0;
                last=strrchr(resolved,'/');if(!last || strcmp(last+1,name))continue;
                snprintf(p->node,sizeof(p->node),"/dev/input/%.32s",e->d_name);
                struct stat st;
                if(!lstat(p->node,&st) && S_ISCHR(st.st_mode)) {p->device=st.st_rdev;closedir(d);return 0;}
            }
            closedir(d);
        }
        struct timespec delay={.tv_nsec=10000000};nanosleep(&delay,NULL);
    }
    errno=ETIMEDOUT;return -1;
}
MatonSessionPads *maton_pads_create(const MatonPadInventory *inventory,size_t count,unsigned owner_uid,
        MatonPadRumble rumble,void *context) {
    if(owner_uid<10000 || owner_uid>=20000 || count>MATON_PAD_LIMIT || (count && !inventory)) {errno=EINVAL;return NULL;}
    MatonSessionPads *s=calloc(1,sizeof(*s));if(!s)return NULL;
    pthread_mutex_init(&s->feedback_mutex,NULL);
    s->count=count;s->rumble=rumble;s->context=context;s->stop[0]=s->stop[1]=-1;
    for(unsigned i=0;i<MATON_PAD_LIMIT;i++){s->pad[i].fd=-1;s->pad[i].playing=-1;}
    for(unsigned i=0;i<count;i++) {
        Pad *p=&s->pad[i];const MatonPadInventory *in=&inventory[i];
        if(!in->descriptor[0] || !memchr(in->descriptor,0,sizeof(in->descriptor)) || in->axis_count>MATON_PAD_AXES) {errno=EINVAL;goto fail;}
        for(unsigned k=0;k<i;k++)if(!strcmp(in->descriptor,inventory[k].descriptor)){errno=EINVAL;goto fail;}
        for(unsigned a=0;a<MATON_PAD_AXES;a++) {
            p->abs[a]=(struct input_absinfo){.minimum=a<4?-32768:a>=6?-1:0,
                .maximum=a<4?32767:a>=6?1:1023,.flat=a<4?4096:0};
        }
        unsigned seen=0;
        for(size_t a=0;a<in->axis_count;a++) {
            unsigned k;for(k=0;k<MATON_PAD_AXES && axes[k]!=in->axes[a].code;k++){}
            struct input_absinfo v=in->axes[a].info;
            if(k==MATON_PAD_AXES || (seen&(1u<<k)) || v.minimum>=v.maximum || (k!=4 && k!=5 && (v.minimum>0 || v.maximum<0)) ||
                    v.flat<0 || v.fuzz<0 || v.resolution<0 || v.value<v.minimum || v.value>v.maximum) {errno=EINVAL;goto fail;}
            seen|=1u<<k;p->abs[k]=v;
        }
        p->fd=open("/dev/uinput",O_RDWR|O_NONBLOCK|O_CLOEXEC);if(p->fd<0)goto fail;
        if(ioctl(p->fd,UI_SET_EVBIT,EV_KEY) || ioctl(p->fd,UI_SET_EVBIT,EV_ABS))goto fail;
        for(size_t k=0;k<sizeof(keys)/sizeof(keys[0]);k++)if(ioctl(p->fd,UI_SET_KEYBIT,keys[k]))goto fail;
        for(unsigned a=0;a<MATON_PAD_AXES;a++) {
            struct uinput_abs_setup u={.code=axes[a],.absinfo=p->abs[a]};
            if(ioctl(p->fd,UI_SET_ABSBIT,axes[a]) || ioctl(p->fd,UI_ABS_SETUP,&u))goto fail;
        }
        int ff=in->rumble && rumble;
        if(ff && (ioctl(p->fd,UI_SET_EVBIT,EV_FF) || ioctl(p->fd,UI_SET_FFBIT,FF_RUMBLE)))goto fail;
        struct uinput_setup u={.id={BUS_USB,0x045e,0x028e,0x0110},.ff_effects_max=ff?EFFECTS:0};
        strcpy(u.name,"Microsoft X-Box 360 pad");
        if(ioctl(p->fd,UI_DEV_SETUP,&u) || ioctl(p->fd,UI_DEV_CREATE))goto fail;
        p->created=1;if(node_for(p))goto fail;
        /* Only our UI_GET_SYSNAME-resolved node changes DAC ownership. */
        if(chown(p->node,owner_uid,owner_uid))goto fail;
        strcat(s->nodes,i?",":"");strcat(s->nodes,p->node);
    }
    if(count && maton_udev_refresh())goto fail;
    if(count) {
        if(pipe2(s->stop,O_CLOEXEC|O_NONBLOCK))goto fail;
        int error=pthread_create(&s->thread,NULL,watch,s);
        if(error){errno=error;goto fail;}s->worker=1;
    }
    return s;
fail:;
    int error=errno;maton_pads_destroy(s);errno=error;return NULL;
}
int maton_pads_send(MatonSessionPads *s,unsigned slot,const struct input_event *events,size_t count) {
    if(!s || slot>=s->count || !events || count>256){errno=EINVAL;return -1;}
    for(size_t i=0;i<count;i++) {
        unsigned code=events[i].code;int value=events[i].value,ok=0;
        if(events[i].type==EV_SYN)ok=code==SYN_REPORT && value==0;
        if(events[i].type==EV_KEY)for(size_t k=0;k<sizeof(keys)/sizeof(keys[0]);k++)if(code==keys[k])ok=value==0 || value==1;
        if(events[i].type==EV_ABS)for(unsigned a=0;a<MATON_PAD_AXES;a++)if(code==axes[a])ok=value>=s->pad[slot].abs[a].minimum && value<=s->pad[slot].abs[a].maximum;
        if(!ok){errno=EINVAL;return -1;}
    }
    for(size_t i=0;i<count;i++) {
        struct input_event e=events[i];memset(&e.time,0,sizeof(e.time));
        ssize_t n;do{n=write(s->pad[slot].fd,&e,sizeof(e));}while(n<0 && errno==EINTR);
        if(n!=sizeof(e)){if(n>=0)errno=EIO;return -1;}
    }
    return 0;
}
int maton_pads_release(MatonSessionPads *s,unsigned slot) {
    struct input_event e[32]={0};size_t n=0;
    if(!s || slot>=s->count){errno=EINVAL;return -1;}
    pthread_mutex_lock(&s->feedback_mutex);cancel(s,slot);pthread_mutex_unlock(&s->feedback_mutex);
    for(size_t k=0;k<sizeof(keys)/sizeof(keys[0]);k++)e[n++]=(struct input_event){.type=EV_KEY,.code=keys[k]};
    for(unsigned a=0;a<MATON_PAD_AXES;a++)e[n++]=(struct input_event){.type=EV_ABS,.code=axes[a],
        .value=a==4 || a==5?s->pad[slot].abs[a].minimum:0};
    e[n++]=(struct input_event){.type=EV_SYN,.code=SYN_REPORT};
    return maton_pads_send(s,slot,e,n);
}
const char *maton_pads_nodes(const MatonSessionPads *s){return s?s->nodes:"";}
void maton_pads_destroy(MatonSessionPads *s) {
    if(!s)return;
    if(s->worker){char stop=1;write(s->stop[1],&stop,1);pthread_join(s->thread,NULL);}
    for(unsigned i=0;i<s->count;i++) {
        Pad *p=&s->pad[i];
        if(p->created){maton_pads_release(s,i);ioctl(p->fd,UI_DEV_DESTROY);}
        if(p->fd>=0)close(p->fd);
    }
    for(unsigned i=0;i<2;i++)if(s->stop[i]>=0)close(s->stop[i]);
    if(s->count) {
        maton_udev_refresh();
        /* A partial unrelated sysfs scan must not preserve our destroyed pads. */
        for(unsigned i=0;i<s->count;i++)if(s->pad[i].device)maton_udev_forget_pad(s->pad[i].device);
    }
    pthread_mutex_destroy(&s->feedback_mutex);free(s);
}
