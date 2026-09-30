#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#include "android_buffer.h"
#include "android_output.h"
#include "compositor_core.h"
#include "us_keymap.h"

#include <android/log.h>
#include <errno.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern void* maton_wayland_test_client(void*);

enum CommandKind { CMD_ATTACH, CMD_DETACH, CMD_RESIZE, CMD_KEY, CMD_MOTION, CMD_RELEASE, CMD_STOP };
struct Command {
  enum CommandKind kind;
  int id, a, b, c;
  int64_t time;
  float x, y, vs, hs;
  struct MatonSurfaceOutput* output;
  struct wlr_buffer* buffer;
  struct Command* next;
};
struct Window {
  int id, width, height, activity;
  struct MatonSurfaceOutput surface;
  struct wlr_output* output;
  struct wlr_scene_output* scene_output;
  struct wlr_scene* scene;
  struct wlr_xdg_toplevel* toplevel;
  struct wl_listener frame, commit, toplevel_destroy;
  struct Window* next;
};
struct Server {
  pthread_mutex_t mutex;
  pthread_cond_t ready_cond;
  pthread_t thread;
  atomic_bool started, stopping, demo_started;
  int event_fd, ready, ok;
  char socket_name[108], runtime_dir[512];
  struct Command* first;
  struct Command* last;
  struct Window* windows;
  struct wl_display* display;
  struct wl_event_loop* loop;
  struct wlr_backend* backend;
  struct wlr_renderer* renderer;
  struct wlr_compositor* compositor;
  struct wlr_allocator* allocator;
  struct MatonAhbAllocator ahb_allocator;
  struct MatonEglUploader uploader;
  struct wlr_xdg_shell* xdg_shell;
  struct wlr_seat* seat;
  struct wlr_keyboard keyboard;
  int keyboard_initialized;
  struct wlr_surface* pointer_focus;
  struct wl_listener new_toplevel;
} server = { .mutex=PTHREAD_MUTEX_INITIALIZER, .ready_cond=PTHREAD_COND_INITIALIZER,
             .event_fd=-1 };

static atomic_int next_window_id = 1;
enum { MATON_BUTTON_LEFT = 1, MATON_BUTTON_RIGHT = 2, MATON_ANDROID_LEFT = 11,
       MATON_ANDROID_RIGHT = 12, BTN_LEFT = 0x110 };
static struct Window* window_from_listener(struct wl_listener* l, size_t offset) {
  return (struct Window*)((char*)l - offset);
}
static struct Window* find_window(int id) {
  for (struct Window* w=server.windows; w; w=w->next) if (w->id==id) return w;
  return NULL;
}
static void enqueue(struct Command* c) {
  pthread_mutex_lock(&server.mutex);
  if (atomic_load(&server.stopping) && c->kind != CMD_STOP) {
    pthread_mutex_unlock(&server.mutex);
    if (c->buffer) wlr_buffer_unlock(c->buffer);
    if (c->output) { maton_surface_output_finish(c->output); free(c->output); }
    free(c); return;
  }
  c->next=NULL;
  if (server.last) server.last->next=c; else server.first=c;
  server.last=c;
  int fd=server.event_fd;
  pthread_mutex_unlock(&server.mutex);
  if (fd>=0) { uint64_t one=1; (void)write(fd,&one,sizeof(one)); }
}
static void command(enum CommandKind kind,int id,int a,int b,int c,int64_t time,
                    float x,float y,float vs,float hs,struct MatonSurfaceOutput* output,
                    struct wlr_buffer* buffer) {
  struct Command* cmd=calloc(1,sizeof(*cmd));
  if (!cmd) { if(buffer)wlr_buffer_unlock(buffer); if(output){maton_surface_output_finish(output);free(output);} return; }
  cmd->kind=kind;cmd->id=id;cmd->a=a;cmd->b=b;cmd->c=c;cmd->time=time;
  cmd->x=x;cmd->y=y;cmd->vs=vs;cmd->hs=hs;cmd->output=output;cmd->buffer=buffer;enqueue(cmd);
}
static void release_buffer(struct wlr_buffer* buffer) {
  command(CMD_RELEASE,0,0,0,0,0,0,0,0,0,NULL,buffer);
}
static void on_commit(struct wl_listener* l,void* data) {
  struct Window* w=window_from_listener(l,offsetof(struct Window,commit));
  struct wlr_output_event_commit* event=data;
  struct wlr_buffer* b=event->state->buffer;
  if(!b)return;
  AHardwareBuffer* ahb=maton_ahb_from_wlr_buffer(b);
  if(!ahb)return;
  if(!maton_ahb_buffer_is_direct(b)&&!maton_ahb_buffer_upload_fallback(b,&server.uploader)){
    __android_log_print(ANDROID_LOG_WARN,"MatonCompositor","AHardwareBuffer upload failed");return;
  }
  (void)maton_surface_output_present(&w->surface,ahb,b,-1);
}
static void on_frame(struct wl_listener* l,void* data) {
  (void)data;
  struct Window* w=window_from_listener(l,offsetof(struct Window,frame));
  if(w->scene_output&&wlr_scene_output_commit(w->scene_output,NULL)){
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);wlr_scene_output_send_frame_done(w->scene_output,&now);
  }
}
static void destroy_output(struct Window* w) {
  if(w->frame.link.prev){wl_list_remove(&w->frame.link);wl_list_init(&w->frame.link);}
  if(w->commit.link.prev){wl_list_remove(&w->commit.link);wl_list_init(&w->commit.link);}
  if(w->scene_output)wlr_scene_output_destroy(w->scene_output);
  if(w->output)wlr_output_destroy(w->output);
  maton_surface_output_finish(&w->surface);w->scene_output=NULL;w->output=NULL;
}
static void destroy_window(struct Window* w) {
  if(w->toplevel_destroy.link.prev){wl_list_remove(&w->toplevel_destroy.link);wl_list_init(&w->toplevel_destroy.link);}
  destroy_output(w);
  if(w->scene)wlr_scene_node_destroy(&w->scene->tree.node);
  struct Window** p=&server.windows;while(*p&&*p!=w)p=&(*p)->next;if(*p)*p=w->next;
  free(w);
}
static void on_toplevel_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,toplevel_destroy));
  wl_list_remove(&l->link);wl_list_init(&l->link);w->toplevel=NULL;w->activity=0;maton_java_close_window(w->id);
}
static void on_new_toplevel(struct wl_listener* l,void* data) {
  (void)l;struct wlr_xdg_toplevel* t=data;struct Window* w=NULL;
  for(struct Window* q=server.windows;q;q=q->next)if(!q->toplevel){w=q;break;}
  int request=0;
  if(!w){w=calloc(1,sizeof(*w));if(!w)return;w->id=atomic_fetch_add(&next_window_id,1);w->width=640;w->height=400;w->scene=wlr_scene_create();if(!w->scene){free(w);return;}w->next=server.windows;server.windows=w;}
  request=!w->activity;w->toplevel=t;w->toplevel_destroy.notify=on_toplevel_destroy;
  wl_signal_add(&t->events.destroy,&w->toplevel_destroy);
  if (!wlr_scene_xdg_surface_create(&w->scene->tree,t->base)) {
    wl_list_remove(&w->toplevel_destroy.link); wl_list_init(&w->toplevel_destroy.link);
    w->toplevel=NULL; return;
  }
  if(!t->base->surface->mapped)wlr_xdg_toplevel_set_size(t,w->width,w->height);
  if(request){w->activity=1;maton_java_request_window(w->id,w->width,w->height);}
}
static void attach_output(struct Command* c) {
  struct Window* w=find_window(c->id);
  if(!c->output)return;
  if(!w){w=calloc(1,sizeof(*w));if(!w){maton_surface_output_finish(c->output);free(c->output);return;}w->id=c->id;w->scene=wlr_scene_create();if(!w->scene){free(w);maton_surface_output_finish(c->output);free(c->output);return;}w->next=server.windows;server.windows=w;}
  if(w->output)destroy_output(w);
  w->width=c->a>0?c->a:(w->width?w->width:640);w->height=c->b>0?c->b:(w->height?w->height:400);
  w->surface=*c->output;free(c->output);w->activity=1;
  w->output=wlr_headless_add_output(server.backend,w->width,w->height);
  if(!w->output||!wlr_output_init_render(w->output,server.allocator,server.renderer)){destroy_output(w);return;}
  char name[48];snprintf(name,sizeof(name),"maton-window-%d",w->id);wlr_output_set_name(w->output,name);
  struct wlr_output_state state;wlr_output_state_init(&state);wlr_output_state_set_enabled(&state,true);
  wlr_output_state_set_custom_mode(&state,w->width,w->height,60000);
  bool ok=wlr_output_commit_state(w->output,&state);wlr_output_state_finish(&state);if(!ok){destroy_output(w);return;}
  w->scene_output=wlr_scene_output_create(w->scene,w->output);if(!w->scene_output){destroy_output(w);return;}
  w->frame.notify=on_frame;wl_signal_add(&w->output->events.frame,&w->frame);
  w->commit.notify=on_commit;wl_signal_add(&w->output->events.commit,&w->commit);wlr_output_schedule_frame(w->output);
}
static void process_commands(void) {
  uint64_t value;while(read(server.event_fd,&value,sizeof(value))>0){}
  pthread_mutex_lock(&server.mutex);struct Command* list=server.first;server.first=server.last=NULL;pthread_mutex_unlock(&server.mutex);
  while(list){struct Command* c=list;list=c->next;struct Window* w=find_window(c->id);
    switch(c->kind){
    case CMD_ATTACH:attach_output(c);break;
    case CMD_DETACH:if(w){w->activity=0;destroy_output(w);}break;
    case CMD_RESIZE:if(w&&c->a>0&&c->b>0){w->width=c->a;w->height=c->b;if(w->output){struct wlr_output_state s;wlr_output_state_init(&s);wlr_output_state_set_custom_mode(&s,c->a,c->b,60000);bool committed=wlr_output_commit_state(w->output,&s);wlr_output_state_finish(&s);if(committed){if(w->toplevel)wlr_xdg_toplevel_set_size(w->toplevel,c->a,c->b);wlr_output_schedule_frame(w->output);}}}break;
    case CMD_KEY:if(server.seat&&server.keyboard_initialized){if(w&&w->toplevel&&w->toplevel->base->surface)wlr_seat_keyboard_notify_enter(server.seat,w->toplevel->base->surface,NULL,0,&server.keyboard.modifiers);struct wlr_keyboard_key_event e={.time_msec=(uint32_t)(c->time/1000000),.keycode=(uint32_t)(c->b>0?c->b:c->a+8),.update_state=true,.state=c->c==0?WL_KEYBOARD_KEY_STATE_PRESSED:WL_KEYBOARD_KEY_STATE_RELEASED};wlr_keyboard_notify_key(&server.keyboard,&e);}break;
    case CMD_MOTION:if(server.seat){uint32_t tm=(uint32_t)(c->time/1000000);double sx=0,sy=0;struct wlr_scene* scene=w?w->scene:NULL;struct wlr_scene_node* node=scene?wlr_scene_node_at(&scene->tree.node,c->x,c->y,&sx,&sy):NULL;struct wlr_scene_buffer* sb=node&&node->type==WLR_SCENE_NODE_BUFFER?wlr_scene_buffer_from_node(node):NULL;struct wlr_scene_surface* ss=sb?wlr_scene_surface_try_from_buffer(sb):NULL;if(ss){if(server.pointer_focus!=ss->surface){server.pointer_focus=ss->surface;wlr_seat_pointer_notify_enter(server.seat,ss->surface,sx,sy);wlr_seat_keyboard_notify_enter(server.seat,ss->surface,NULL,0,&server.keyboard.modifiers);}wlr_seat_pointer_notify_motion(server.seat,tm,sx,sy);}else{server.pointer_focus=NULL;wlr_seat_pointer_notify_clear_focus(server.seat);}if(c->a==MATON_ANDROID_LEFT||c->a==MATON_ANDROID_RIGHT||c->a==MATON_BUTTON_LEFT||c->a==MATON_BUTTON_RIGHT)wlr_seat_pointer_notify_button(server.seat,tm,BTN_LEFT,(c->a==MATON_ANDROID_LEFT||c->a==MATON_BUTTON_LEFT)?WL_POINTER_BUTTON_STATE_PRESSED:WL_POINTER_BUTTON_STATE_RELEASED);if(c->vs)wlr_seat_pointer_notify_axis(server.seat,tm,WL_POINTER_AXIS_VERTICAL_SCROLL,-c->vs*15.0,(int32_t)-c->vs,WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);if(c->hs)wlr_seat_pointer_notify_axis(server.seat,tm,WL_POINTER_AXIS_HORIZONTAL_SCROLL,-c->hs*15.0,(int32_t)-c->hs,WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);wlr_seat_pointer_notify_frame(server.seat);}break;
    case CMD_RELEASE:if(c->buffer)wlr_buffer_unlock(c->buffer);break;
    case CMD_STOP:atomic_store(&server.stopping,true);break;
    }free(c);
  }
}
static int on_event_fd(int fd,uint32_t mask,void* data){(void)fd;(void)mask;(void)data;process_commands();return 0;}
static void* server_main(void* unused) {
  (void)unused;bool initialized=false;wlr_log_init(WLR_ERROR,NULL);server.display=wl_display_create();
  if(!server.display)goto done;server.loop=wl_display_get_event_loop(server.display);
  maton_surface_output_set_release_dispatch(release_buffer);
  server.event_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
  if(server.event_fd<0||!wl_event_loop_add_fd(server.loop,server.event_fd,WL_EVENT_READABLE,on_event_fd,NULL))goto done;
  server.backend=wlr_headless_backend_create(server.loop);server.renderer=wlr_pixman_renderer_create();
  if(!server.backend||!server.renderer||!maton_ahb_allocator_init(&server.ahb_allocator))goto done;
  if(!wlr_renderer_init_wl_display(server.renderer,server.display))goto done;
  server.allocator=maton_ahb_allocator_base(&server.ahb_allocator);
  if(!maton_egl_uploader_init(&server.uploader))__android_log_print(ANDROID_LOG_WARN,"MatonCompositor","EGL upload fallback unavailable");
  server.xdg_shell=wlr_xdg_shell_create(server.display,6);server.compositor=wlr_compositor_create(server.display,6,server.renderer);server.seat=wlr_seat_create(server.display,"seat0");
  if(!server.xdg_shell||!server.compositor||!server.seat)goto done;
  wlr_keyboard_init(&server.keyboard,NULL,"maton-keyboard");server.keyboard_initialized=1;
  struct xkb_context* xc=xkb_context_new(XKB_CONTEXT_NO_FLAGS);struct xkb_keymap* km=xc?xkb_keymap_new_from_string(xc,kMatonUsKeymap,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS):NULL;
  if(km){(void)wlr_keyboard_set_keymap(&server.keyboard,km);xkb_keymap_unref(km);}if(xc)xkb_context_unref(xc);
  wlr_seat_set_keyboard(server.seat,&server.keyboard);wlr_seat_set_capabilities(server.seat,WL_SEAT_CAPABILITY_KEYBOARD|WL_SEAT_CAPABILITY_POINTER);
  server.new_toplevel.notify=on_new_toplevel;wl_signal_add(&server.xdg_shell->events.new_toplevel,&server.new_toplevel);
  if(!wl_display_add_socket(server.display,server.socket_name)||!wlr_backend_start(server.backend))goto done;
  initialized=true;
done:
  pthread_mutex_lock(&server.mutex);server.ok=initialized;server.ready=1;pthread_cond_broadcast(&server.ready_cond);pthread_mutex_unlock(&server.mutex);
  if(server.ok){while(!atomic_load(&server.stopping))wl_event_loop_dispatch(server.loop,-1);wl_display_destroy_clients(server.display);}
  while(server.windows)destroy_window(server.windows);
  if(server.new_toplevel.link.prev){wl_list_remove(&server.new_toplevel.link);wl_list_init(&server.new_toplevel.link);}
  if(server.keyboard_initialized)wlr_keyboard_finish(&server.keyboard);server.keyboard_initialized=0;
  if(server.seat)wlr_seat_destroy(server.seat);server.seat=NULL;
  if(server.allocator)wlr_allocator_destroy(server.allocator);server.allocator=NULL;
  if(server.renderer)wlr_renderer_destroy(server.renderer);server.renderer=NULL;
  maton_egl_uploader_finish(&server.uploader);if(server.backend)wlr_backend_destroy(server.backend);server.backend=NULL;
  if(server.display)wl_display_destroy(server.display);server.display=NULL;server.loop=NULL;
  if(server.event_fd>=0)close(server.event_fd);server.event_fd=-1;return NULL;
}
bool maton_core_start(const char* socket_name,const char* runtime_dir){
  bool expected=false;if(!atomic_compare_exchange_strong(&server.started,&expected,true)){pthread_mutex_lock(&server.mutex);bool ok=server.ok;pthread_mutex_unlock(&server.mutex);return ok;}
  if(!socket_name||!runtime_dir||strlen(socket_name)>=sizeof(server.socket_name)||strlen(runtime_dir)>=sizeof(server.runtime_dir))goto fail;
  strcpy(server.socket_name,socket_name);strcpy(server.runtime_dir,runtime_dir);
  if(mkdir(runtime_dir,0700)&&errno!=EEXIST)goto fail;if(chmod(runtime_dir,0700)||setenv("XDG_RUNTIME_DIR",runtime_dir,1))goto fail;
  char path[640];snprintf(path,sizeof(path),"%s/%s",runtime_dir,socket_name);unlink(path);
  pthread_mutex_lock(&server.mutex);server.ready=server.ok=0;pthread_mutex_unlock(&server.mutex);
  atomic_store(&server.stopping,false);
  if(pthread_create(&server.thread,NULL,server_main,NULL))goto fail;
  pthread_mutex_lock(&server.mutex);struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);limit.tv_sec+=2;while(!server.ready&&pthread_cond_timedwait(&server.ready_cond,&server.mutex,&limit)==0){}int ok=server.ready&&server.ok;pthread_mutex_unlock(&server.mutex);if(!ok){pthread_join(server.thread,NULL);pthread_mutex_lock(&server.mutex);server.started=false;server.ready=server.ok=0;pthread_mutex_unlock(&server.mutex);}return ok;
fail:atomic_store(&server.started,false);return false;
}
void maton_core_stop(void){if(!atomic_load(&server.started))return;atomic_store(&server.stopping,true);if(server.event_fd>=0){uint64_t one=1;(void)write(server.event_fd,&one,sizeof(one));}pthread_join(server.thread,NULL);pthread_mutex_lock(&server.mutex);server.ready=server.ok=0;atomic_store(&server.started,false);pthread_mutex_unlock(&server.mutex);atomic_store(&server.stopping,false);atomic_store(&server.demo_started,false);}
void maton_core_launch_demo(void){bool expected=false;if(atomic_compare_exchange_strong(&server.demo_started,&expected,true)){pthread_t t;if(!pthread_create(&t,NULL,maton_wayland_test_client,NULL))pthread_detach(t);}}
void maton_core_attach(int id,struct MatonSurfaceOutput* output,int width,int height){command(CMD_ATTACH,id,width,height,0,0,0,0,0,0,output,NULL);}
void maton_core_detach(int id){command(CMD_DETACH,id,0,0,0,0,0,0,0,0,NULL,NULL);}
void maton_core_resize(int id,int width,int height){command(CMD_RESIZE,id,width,height,0,0,0,0,0,0,NULL,NULL);}
void maton_core_key(int id,int keycode,int scan,int action,int meta,int64_t time){(void)meta;command(CMD_KEY,id,keycode,scan,action,time,0,0,0,0,NULL,NULL);}
void maton_core_motion(int id,float x,float y,float vs,float hs,int action,int buttons,int64_t time){(void)buttons;command(CMD_MOTION,id,action,0,0,time,x,y,vs,hs,NULL,NULL);}
