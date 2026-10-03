#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#include "android_buffer.h"
#include "android_output.h"
#include "compositor_core.h"
#include "us_keymap.h"

#include <android/log.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/render/drm_format_set.h>
#include <dirent.h>
#include <math.h>
#include <sys/stat.h>
#include "dmabuf_import.h"
#include "maton_renderer.h"
#include "presenter.h"
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "pointer_buttons.h"

extern void* maton_wayland_test_client(void*);

/* Route wlroots logging into logcat: stderr is lost in app processes, and
 * Xwayland spawn or XWM failures would otherwise be invisible. */
static void maton_wlr_log(enum wlr_log_importance importance,const char* fmt,va_list args) {
  /* wlroots orders SILENT < ERROR < INFO < DEBUG (higher is chattier). */
  int priority=importance<=WLR_ERROR?ANDROID_LOG_ERROR:importance==WLR_INFO?ANDROID_LOG_INFO:ANDROID_LOG_DEBUG;
  __android_log_vprint(priority,"MatonWLR",fmt,args);
}

struct SocketSession {
  int id, fd;
  char path[108];
  struct SocketSession* next;
};
struct XwaylandSession {
  int session;
  uid_t uid;
  struct wlr_xwayland* xwayland;
  struct wl_listener ready, new_surface, destroy;
  struct XwaylandSession* next;
};
enum CommandKind { CMD_ATTACH, CMD_DETACH, CMD_RESIZE, CMD_KEY, CMD_MOTION, CMD_RELEASE, CMD_CLOSE, CMD_STOP, CMD_ADD_SESSION, CMD_ADD_XWAYLAND };
struct Command {
  enum CommandKind kind;
  int id, a, b, c;
  int64_t time;
  float x, y, vs, hs;
  struct MatonSurfaceOutput* output;
  struct wlr_buffer* buffer;
  struct SocketSession* session;
  struct Command* next;
};
struct Window {
  int id, width, height, activity, session;
  struct MatonSurfaceOutput surface;
  struct wlr_output* output;
  struct wlr_scene_output* scene_output;
  struct wlr_scene* scene;
  struct wlr_xdg_toplevel* toplevel;
  struct wlr_xwayland_surface* xsurface;
  struct wlr_scene_tree* scene_tree;
  struct MatonPresenter* presenter;
  struct wl_listener frame, commit, toplevel_destroy, surface_commit;
  struct wl_listener xsurface_destroy, xsurface_associate, xsurface_geometry, scene_tree_destroy;
  struct Window* next;
};
struct Server {
  pthread_mutex_t mutex;
  pthread_cond_t ready_cond;
  pthread_cond_t xw_cond;
  pthread_t thread;
  atomic_bool started, stopping, demo_started;
  int event_fd, ready, ok;
  char socket_name[108], runtime_dir[512];
  struct Command* first;
  struct Command* last;
  struct Window* windows;
  struct SocketSession* sessions;
  bool xw_done; unsigned xw_sent, xw_completed; bool xw_ok; char xw_display[128];
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
  uint32_t pointer_buttons;
  int pointer_window;
  struct wl_listener new_toplevel, new_popup, new_decoration;
  char xwayland_path[512], xwayland_dir[512];
  struct XwaylandSession* xwayland_sessions;
  struct wlr_output* monitor;
  struct wlr_drm_format_set dmabuf_formats;
} server = { .mutex=PTHREAD_MUTEX_INITIALIZER, .ready_cond=PTHREAD_COND_INITIALIZER,
             .xw_cond=PTHREAD_COND_INITIALIZER,
             .event_fd=-1 };

/* WindowActivity's built-in demo reserves ID 1. Real application windows
 * must never share it, including when the demo is opened after an app. */
static atomic_int next_window_id = 2;
static void handle_add_xwayland(int session,uid_t uid);
static void on_xsurface_commit(struct wl_listener* l,void* data);
struct PointerButtonEvent { uint32_t time; int window; };
static void notify_pointer_button(void* data, uint32_t code, bool pressed) {
  struct PointerButtonEvent* event = data;
  if(__android_log_is_loggable(ANDROID_LOG_DEBUG,"MatonPointer",ANDROID_LOG_INFO))
    __android_log_print(ANDROID_LOG_DEBUG,"MatonPointer","Window %d: button %u %s",event->window,code,pressed ? "pressed" : "released");
  wlr_seat_pointer_notify_button(server.seat,event->time,code,
      pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
}
static void clear_window_pointer(struct Window* w) {
  if(!w || !server.seat || server.pointer_window!=w->id)return;
  struct timespec now={0};clock_gettime(CLOCK_MONOTONIC,&now);
  struct PointerButtonEvent event={.time=(uint32_t)(now.tv_sec*1000+now.tv_nsec/1000000),.window=w->id};
  maton_pointer_buttons_update(&server.pointer_buttons,0,notify_pointer_button,&event);
  wlr_seat_pointer_notify_clear_focus(server.seat);
  wlr_seat_pointer_notify_frame(server.seat);
  server.pointer_focus=NULL;server.pointer_window=0;
}
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
    if (c->session) { close(c->session->fd); unlink(c->session->path); free(c->session); }
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
  /* Windows are not composited: each visible buffer becomes its own Android
   * layer (presenter.c), so client dma-bufs reach SurfaceFlinger untouched. */
  if(w->scene_output&&w->presenter){
    maton_presenter_present(w->presenter,w->scene);
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);wlr_scene_output_send_frame_done(w->scene_output,&now);
  }
}
static void destroy_output(struct Window* w) {
  clear_window_pointer(w);
  if(w->frame.link.prev){wl_list_remove(&w->frame.link);wl_list_init(&w->frame.link);}
  if(w->commit.link.prev){wl_list_remove(&w->commit.link);wl_list_init(&w->commit.link);}
  if(w->presenter)maton_presenter_destroy(w->presenter);w->presenter=NULL;
  if(w->scene_output)wlr_scene_output_destroy(w->scene_output);
  if(w->output)wlr_output_destroy(w->output);
  maton_surface_output_finish(&w->surface);w->scene_output=NULL;w->output=NULL;
}
static void destroy_window(struct Window* w) {
  if(w->surface_commit.link.prev){wl_list_remove(&w->surface_commit.link);wl_list_init(&w->surface_commit.link);}
  if(w->toplevel_destroy.link.prev){wl_list_remove(&w->toplevel_destroy.link);wl_list_init(&w->toplevel_destroy.link);}
  if(w->xsurface_destroy.link.prev){wl_list_remove(&w->xsurface_destroy.link);wl_list_init(&w->xsurface_destroy.link);}
  if(w->xsurface_associate.link.prev){wl_list_remove(&w->xsurface_associate.link);wl_list_init(&w->xsurface_associate.link);}
  if(w->xsurface_geometry.link.prev){wl_list_remove(&w->xsurface_geometry.link);wl_list_init(&w->xsurface_geometry.link);}
  destroy_output(w);
  if(w->scene)wlr_scene_node_destroy(&w->scene->tree.node);
  struct Window** p=&server.windows;while(*p&&*p!=w)p=&(*p)->next;if(*p)*p=w->next;
  free(w);
}
static void on_toplevel_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,toplevel_destroy));
  wl_list_remove(&l->link);wl_list_init(&l->link);
  wl_list_remove(&w->surface_commit.link);wl_list_init(&w->surface_commit.link);
  destroy_output(w);w->toplevel=NULL;w->activity=0;maton_java_close_window(w->id);
}
/* Keyboard focus belongs to the window, never to the popup or subsurface
 * under the pointer: moving it there makes toolkits see a focus-out on the
 * toplevel and dismiss their open menus. */
static struct wlr_surface* window_root_surface(struct Window* w) {
  if(w->toplevel&&w->toplevel->base->surface)return w->toplevel->base->surface;
  if(w->xsurface&&w->xsurface->surface)return w->xsurface->surface;
  return NULL;
}
static void on_surface_commit(struct wl_listener* l,void* data) {
  (void)data;
  struct Window* w=window_from_listener(l,offsetof(struct Window,surface_commit));
  /* wlroots initializes xdg surfaces on their first surface commit. */
  if(w->toplevel&&w->toplevel->base->initial_commit){
    wlr_xdg_toplevel_set_size(w->toplevel,w->width,w->height);
    /* Android frames and manages the window, so it is tiled on every edge:
     * toolkits then drop rounded corners, shadow margins and resize borders.
     * Firefox ignores tiled for its own frame, so maximized is sent as well;
     * the window size stays whatever Android gives it. */
    wlr_xdg_toplevel_set_tiled(w->toplevel,WLR_EDGE_TOP|WLR_EDGE_BOTTOM|WLR_EDGE_LEFT|WLR_EDGE_RIGHT);
    wlr_xdg_toplevel_set_maximized(w->toplevel,true);
  }
}
static void on_new_toplevel(struct wl_listener* l,void* data) {
  (void)l;struct wlr_xdg_toplevel* t=data;struct Window* w=NULL;
  int session=0;
  struct wl_client* client=wl_resource_get_client(t->base->surface->resource);
  struct sockaddr_un address={0};socklen_t size=sizeof(address);
  if(getsockname(wl_client_get_fd(client),(struct sockaddr*)&address,&size)==0)
    for(struct SocketSession* s=server.sessions;s;s=s->next)
      if(!strcmp(address.sun_path,s->path)){session=s->id;break;}
  for(struct Window* q=server.windows;q;q=q->next)if(!q->toplevel&&q->session==session){w=q;break;}
  int request=0;
  if(!w){w=calloc(1,sizeof(*w));if(!w)return;w->id=atomic_fetch_add(&next_window_id,1);w->width=640;w->height=400;w->scene=wlr_scene_create();if(!w->scene){free(w);return;}w->next=server.windows;server.windows=w;}
  request=!w->activity;w->session=session;w->toplevel=t;w->toplevel_destroy.notify=on_toplevel_destroy;
  wl_signal_add(&t->events.destroy,&w->toplevel_destroy);
  struct wlr_scene_tree* tree=wlr_scene_xdg_surface_create(&w->scene->tree,t->base);
  t->base->data=tree;
  if (!tree) {
    wl_list_remove(&w->toplevel_destroy.link); wl_list_init(&w->toplevel_destroy.link);
    w->toplevel=NULL; return;
  }
  w->surface_commit.notify=on_surface_commit;
  wl_signal_add(&t->base->surface->events.commit,&w->surface_commit);
  if(request){w->activity=1;maton_java_request_window(session,w->id,w->width,w->height);}
}
struct Popup {
  struct wlr_xdg_popup* popup;
  struct wl_listener commit, reposition, destroy;
};
/* Each window is its own Android output sized to the window, so anything a
 * popup draws past the window edge is cut off. Popups are therefore
 * constrained to the window: the positioner rules flip or slide menus back
 * inside it, as a desktop compositor does at the screen edge. */
static void constrain_popup(struct wlr_xdg_popup* popup) {
  struct wlr_surface* surface=popup->parent;struct wlr_xdg_surface* xdg;
  while((xdg=wlr_xdg_surface_try_from_wlr_surface(surface))&&xdg->role==WLR_XDG_SURFACE_ROLE_POPUP&&xdg->popup)
    surface=xdg->popup->parent;
  for(struct Window* w=server.windows;w;w=w->next){
    if(!xdg||!w->toplevel||w->toplevel->base!=xdg)continue;
    /* The scene places the window geometry, not the surface, at the output origin. */
    struct wlr_box box={xdg->geometry.x,xdg->geometry.y,w->width,w->height};
    wlr_xdg_popup_unconstrain_from_box(popup,&box);return;
  }
  wlr_xdg_surface_schedule_configure(popup->base);
}
static void on_popup_commit(struct wl_listener* l,void* data) {
  (void)data;struct Popup* p=(struct Popup*)((char*)l - offsetof(struct Popup,commit));
  if(p->popup->base->initial_commit)constrain_popup(p->popup);
}
static void on_popup_reposition(struct wl_listener* l,void* data) {
  (void)data;struct Popup* p=(struct Popup*)((char*)l - offsetof(struct Popup,reposition));
  constrain_popup(p->popup);
}
static void on_popup_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Popup* p=(struct Popup*)((char*)l - offsetof(struct Popup,destroy));
  wl_list_remove(&p->commit.link);wl_list_remove(&p->reposition.link);wl_list_remove(&p->destroy.link);free(p);
}
static void on_new_popup(struct wl_listener* listener,void* data) {
  (void)listener;struct wlr_xdg_popup* popup=data;
  struct wlr_xdg_surface* parent=wlr_xdg_surface_try_from_wlr_surface(popup->parent);
  if(!parent||!parent->data)return;
  popup->base->data=wlr_scene_xdg_surface_create(parent->data,popup->base);
  if(!popup->base->data)return;
  struct Popup* p=calloc(1,sizeof(*p));if(!p)return;p->popup=popup;
  p->commit.notify=on_popup_commit;wl_signal_add(&popup->base->surface->events.commit,&p->commit);
  p->reposition.notify=on_popup_reposition;wl_signal_add(&popup->events.reposition,&p->reposition);
  p->destroy.notify=on_popup_destroy;wl_signal_add(&popup->events.destroy,&p->destroy);
}
/* Android's caption bar is the only window decoration MatonOS provides, so
 * client-side decoration requests are refused: every toplevel is told that
 * the server decorates. Toolkits then drop their own title bars instead of
 * drawing a second one below Android's caption. */
struct Decoration {
  struct wlr_xdg_toplevel_decoration_v1* deco;
  struct wl_listener request, destroy;
};
static void on_decoration_request(struct wl_listener* l,void* data) {
  (void)data;struct Decoration* d=(struct Decoration*)((char*)l-offsetof(struct Decoration,request));
  wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void on_decoration_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Decoration* d=(struct Decoration*)((char*)l-offsetof(struct Decoration,destroy));
  wl_list_remove(&d->request.link);wl_list_remove(&d->destroy.link);free(d);
}
static void on_new_decoration(struct wl_listener* l,void* data) {
  (void)l;struct wlr_xdg_toplevel_decoration_v1* deco=data;
  struct Decoration* d=calloc(1,sizeof(*d));if(!d)return;
  d->deco=deco;
  d->request.notify=on_decoration_request;wl_signal_add(&deco->events.request_mode,&d->request);
  d->destroy.notify=on_decoration_destroy;wl_signal_add(&deco->events.destroy,&d->destroy);
  wlr_xdg_toplevel_decoration_v1_set_mode(deco,WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
/* Xwayland windows reuse the toplevel window path: one Android activity per
 * managed X window, rendered through a scene subsurface tree. */
static void on_xsurface_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,xsurface_destroy));
  wl_list_remove(&l->link);wl_list_init(&l->link);
  wl_list_remove(&w->surface_commit.link);wl_list_init(&w->surface_commit.link);
  if(w->xsurface_associate.link.prev){wl_list_remove(&w->xsurface_associate.link);wl_list_init(&w->xsurface_associate.link);}
  if(w->xsurface_geometry.link.prev){wl_list_remove(&w->xsurface_geometry.link);wl_list_init(&w->xsurface_geometry.link);}
  clear_window_pointer(w);destroy_output(w);w->xsurface=NULL;w->activity=0;
  maton_java_close_window(w->id);
}
/* The X window manager only lets a client move focus (e.g. to its menu)
 * between windows of the app it last activated; with none activated every
 * change is reverted and toolkits close their menus on the focus loss. */
static void xwindow_activate(struct Window* w) {
  if(w&&w->xsurface)wlr_xwayland_surface_activate(w->xsurface,true);
}
/* wlroots destroys the tree itself when the surface goes (client exit). */
static void on_scene_tree_destroy(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,scene_tree_destroy));
  wl_list_remove(&l->link);wl_list_init(&l->link);
  w->scene_tree=NULL;
}
static void xwindow_show_surface(struct Window* w) {
  if(!w->xsurface||!w->xsurface->surface)return;
  /* The scene renders the paired Wayland surface; Android owns presentation. */
  if(!w->scene_tree&&w->scene){
    w->scene_tree=wlr_scene_subsurface_tree_create(&w->scene->tree,w->xsurface->surface);
    if(w->scene_tree){w->scene_tree_destroy.notify=on_scene_tree_destroy;wl_signal_add(&w->scene_tree->node.events.destroy,&w->scene_tree_destroy);}
  }
  /* X11 has no tiled state; maximized makes toolkits drop frame effects. */
  wlr_xwayland_surface_set_maximized(w->xsurface,true,true);
  xwindow_activate(w);
  /* Overlay positions and pointer input assume every managed X window sits
   * at the X origin; the client may have placed it elsewhere before map. */
  wlr_xwayland_surface_configure(w->xsurface,0,0,w->width,w->height);
}
static void on_xsurface_geometry(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,xsurface_geometry));
  /* Android's window size is authoritative: acknowledge X geometry requests
   * with our configured size so the client redraws to fit the activity. */
  if(w->xsurface)
    wlr_xwayland_surface_configure(w->xsurface,0,0,w->width,w->height);
}
static struct Window* xwayland_window(struct wlr_xwayland_surface* xs,int session) {
  for(struct Window* w=server.windows;w;w=w->next)if(w->xsurface==xs)return w;
  struct Window* w=calloc(1,sizeof(*w));if(!w)return NULL;
  w->id=atomic_fetch_add(&next_window_id,1);
  w->width=xs->width>0?(int)xs->width:640;w->height=xs->height>0?(int)xs->height:400;
  w->scene=wlr_scene_create();
  if(!w->scene){free(w);return NULL;}
  w->next=server.windows;server.windows=w;
  w->session=session;w->xsurface=xs;
  w->xsurface_destroy.notify=on_xsurface_destroy;
  wl_signal_add(&xs->events.destroy,&w->xsurface_destroy);
  w->xsurface_geometry.notify=on_xsurface_geometry;
  wl_signal_add(&xs->events.set_geometry,&w->xsurface_geometry);
  w->surface_commit.notify=on_xsurface_commit;
  wl_signal_add(&xs->events.request_configure,&w->surface_commit);
  w->activity=1;maton_java_request_window(session,w->id,w->width,w->height);
  xwindow_show_surface(w);
  return w;
}
static void on_xsurface_commit(struct wl_listener* l,void* data) {
  (void)data;struct Window* w=window_from_listener(l,offsetof(struct Window,surface_commit));
  if(w->xsurface)wlr_xwayland_surface_configure(w->xsurface,0,0,w->width,w->height);
}
/* Override-redirect windows (menus, tooltips, drop-downs) get no Android
 * window of their own. Every managed X window is configured at the X origin,
 * so their root coordinates are offsets within the window they belong to:
 * the transient parent if set, else the window under the pointer, else the
 * session's newest X window. */
struct XOverlay {
  struct wlr_xwayland_surface* xs;
  struct wlr_scene_tree* tree;
  int session;
  struct wl_listener associate, dissociate, geometry, destroy, tree_destroy;
};
static struct Window* xoverlay_owner(struct XOverlay* o) {
  for(struct wlr_xwayland_surface* p=o->xs->parent;p;p=p->parent)
    for(struct Window* w=server.windows;w;w=w->next)if(w->xsurface==p)return w;
  struct Window* newest=NULL;
  for(struct Window* w=server.windows;w;w=w->next){
    if(!w->xsurface||w->session!=o->session||!w->scene)continue;
    if(w->id==server.pointer_window)return w;
    if(!newest||w->id>newest->id)newest=w;
  }
  return newest;
}
/* The tree can also die with its owner window's scene; forget it then. */
static void on_xoverlay_tree_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,tree_destroy));
  wl_list_remove(&o->tree_destroy.link);wl_list_init(&o->tree_destroy.link);
  o->tree=NULL;
}
static void xoverlay_remove_tree(struct XOverlay* o) {
  if(o->tree)wlr_scene_node_destroy(&o->tree->node);
}
static void on_xoverlay_associate(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,associate));
  struct Window* w=xoverlay_owner(o);
  if(!w||!o->xs->surface)return;
  xoverlay_remove_tree(o);
  o->tree=wlr_scene_subsurface_tree_create(&w->scene->tree,o->xs->surface);
  if(!o->tree)return;
  o->tree_destroy.notify=on_xoverlay_tree_destroy;wl_signal_add(&o->tree->node.events.destroy,&o->tree_destroy);
  wlr_scene_node_raise_to_top(&o->tree->node);
  wlr_scene_node_set_position(&o->tree->node,o->xs->x,o->xs->y);
  __android_log_print(ANDROID_LOG_DEBUG,"MatonCompositor","X overlay %ux%u+%d+%d class=%s on window %d",
      o->xs->width,o->xs->height,o->xs->x,o->xs->y,o->xs->class?o->xs->class:"-",w->id);
  if(w->output)wlr_output_schedule_frame(w->output);
}
static void on_xoverlay_dissociate(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,dissociate));
  xoverlay_remove_tree(o);
}
static void on_xoverlay_geometry(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,geometry));
  if(o->tree)wlr_scene_node_set_position(&o->tree->node,o->xs->x,o->xs->y);
}
static void on_xoverlay_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XOverlay* o=(struct XOverlay*)((char*)l-offsetof(struct XOverlay,destroy));
  xoverlay_remove_tree(o);
  wl_list_remove(&o->associate.link);wl_list_remove(&o->dissociate.link);
  wl_list_remove(&o->geometry.link);wl_list_remove(&o->destroy.link);
  free(o);
}
static void xwayland_overlay(struct wlr_xwayland_surface* xs,int session) {
  struct XOverlay* o=calloc(1,sizeof(*o));if(!o)return;
  o->xs=xs;o->session=session;wl_list_init(&o->tree_destroy.link);
  o->associate.notify=on_xoverlay_associate;wl_signal_add(&xs->events.associate,&o->associate);
  o->dissociate.notify=on_xoverlay_dissociate;wl_signal_add(&xs->events.dissociate,&o->dissociate);
  o->geometry.notify=on_xoverlay_geometry;wl_signal_add(&xs->events.set_geometry,&o->geometry);
  o->destroy.notify=on_xoverlay_destroy;wl_signal_add(&xs->events.destroy,&o->destroy);
}
/* Toolkits create many managed X windows that are never shown (Chromium's
 * helper and InputOnly windows). An Android window exists only while the X
 * window is mapped; until then its own configure requests are honored. */
struct XTrack {
  struct wlr_xwayland_surface* xs;
  int session;
  struct wl_listener associate, dissociate, configure, destroy;
};
static struct Window* xtrack_window(struct XTrack* t) {
  for(struct Window* w=server.windows;w;w=w->next)if(w->xsurface==t->xs)return w;
  return NULL;
}
static void on_xtrack_associate(struct wl_listener* l,void* data) {
  (void)data;struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,associate));
  if(!t->xs->override_redirect&&!xtrack_window(t))xwayland_window(t->xs,t->session);
}
static void on_xtrack_dissociate(struct wl_listener* l,void* data) {
  (void)data;struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,dissociate));
  struct Window* w=xtrack_window(t);
  if(!w)return;
  if(w->scene_tree){wlr_scene_node_destroy(&w->scene_tree->node);w->scene_tree=NULL;}
  on_xsurface_destroy(&w->xsurface_destroy,NULL);
}
static void on_xtrack_configure(struct wl_listener* l,void* data) {
  struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,configure));
  struct wlr_xwayland_surface_configure_event* ev=data;
  if(!xtrack_window(t))wlr_xwayland_surface_configure(t->xs,ev->x,ev->y,ev->width,ev->height);
}
static void on_xtrack_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XTrack* t=(struct XTrack*)((char*)l-offsetof(struct XTrack,destroy));
  wl_list_remove(&t->associate.link);wl_list_remove(&t->dissociate.link);
  wl_list_remove(&t->configure.link);wl_list_remove(&t->destroy.link);
  free(t);
}
static void xwayland_track(struct wlr_xwayland_surface* xs,int session) {
  struct XTrack* t=calloc(1,sizeof(*t));if(!t)return;
  t->xs=xs;t->session=session;
  t->associate.notify=on_xtrack_associate;wl_signal_add(&xs->events.associate,&t->associate);
  t->dissociate.notify=on_xtrack_dissociate;wl_signal_add(&xs->events.dissociate,&t->dissociate);
  t->configure.notify=on_xtrack_configure;wl_signal_add(&xs->events.request_configure,&t->configure);
  t->destroy.notify=on_xtrack_destroy;wl_signal_add(&xs->events.destroy,&t->destroy);
}
static void on_xwayland_new_surface(struct wl_listener* l,void* data) {
  struct XwaylandSession* xw=(struct XwaylandSession*)((char*)l-offsetof(struct XwaylandSession,new_surface));
  struct wlr_xwayland_surface* xs=data;
  if(xs->override_redirect)xwayland_overlay(xs,xw->session);
  else xwayland_track(xs,xw->session);
}
static void xwayland_apply_socket_mode(struct XwaylandSession* xw) {
  if(!xw->xwayland)return;
  const char* dir=getenv("WLR_XWAYLAND_SOCKET_DIR");
  if(!dir||!dir[0])return;
  char path[256];snprintf(path,sizeof(path),"%s/X%s",dir,xw->xwayland->display_name+1);
  struct stat info;
  if(stat(path,&info)==0){
    (void)chmod(path,0666);
  }
}
static void on_xwayland_ready(struct wl_listener* l,void* data) {
  (void)data;struct XwaylandSession* xw=(struct XwaylandSession*)((char*)l-offsetof(struct XwaylandSession,ready));
  __android_log_print(ANDROID_LOG_INFO,"MatonCompositor","Session %d: Xwayland ready on %s",
      xw->session,xw->xwayland?xw->xwayland->display_name:"?");
  xwayland_apply_socket_mode(xw);
}
static void on_xwayland_destroy(struct wl_listener* l,void* data) {
  (void)data;struct XwaylandSession* xw=(struct XwaylandSession*)((char*)l-offsetof(struct XwaylandSession,destroy));
  wl_list_remove(&xw->ready.link);wl_list_remove(&xw->new_surface.link);wl_list_remove(&xw->destroy.link);
  struct XwaylandSession** p=&server.xwayland_sessions;
  while(*p&&*p!=xw)p=&(*p)->next;if(*p)*p=xw->next;
  free(xw);
}
/* The single advertised monitor is what clients take as the screen: GTK and
 * Firefox place and track popups within it, and it is Xwayland's root window,
 * to which the X pointer is clamped. It starts at the display's size (sent
 * from Java as a resize of window 0) and grows to cover any larger Android
 * window; it never shrinks, which would push content past the root edge. */
static void monitor_cover(int width,int height) {
  struct wlr_output* m=server.monitor;
  if(!m||(width<=m->width&&height<=m->height))return;
  int w=width>m->width?width:m->width,h=height>m->height?height:m->height;
  struct wlr_output_state state;wlr_output_state_init(&state);
  wlr_output_state_set_custom_mode(&state,w,h,60000);
  if(!wlr_output_commit_state(m,&state))
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Monitor resize to %dx%d failed",w,h);
  wlr_output_state_finish(&state);
}
static void attach_output(struct Command* c) {
  struct Window* w=find_window(c->id);
  if(!c->output)return;
  if(!w){w=calloc(1,sizeof(*w));if(!w){maton_surface_output_finish(c->output);free(c->output);return;}w->id=c->id;w->scene=wlr_scene_create();if(!w->scene){free(w);maton_surface_output_finish(c->output);free(c->output);return;}w->next=server.windows;server.windows=w;}
  if(w->output)destroy_output(w);
  w->width=c->a>0?c->a:(w->width?w->width:640);w->height=c->b>0?c->b:(w->height?w->height:400);
  w->surface=*c->output;free(c->output);w->activity=1;
  w->output=wlr_headless_add_output(server.backend,w->width,w->height);
  if(!w->output||!wlr_output_init_render(w->output,server.allocator,server.renderer)){
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Window %d: output renderer initialization failed",w->id);
    destroy_output(w);return;
  }
  char name[48];snprintf(name,sizeof(name),"maton-window-%d",w->id);wlr_output_set_name(w->output,name);
  /* Android presentation needs our AHardwareBuffer, not a Wayland client's
   * SHM buffer submitted directly by the scene's scanout optimization. */
  wlr_output_lock_attach_render(w->output,true);
  struct wlr_output_state state;wlr_output_state_init(&state);wlr_output_state_set_enabled(&state,true);
  /* The AHardwareBuffer allocator supplies BGRA with alpha. wlroots defaults
   * to XRGB, which this allocator cannot create even for the initial clear. */
  wlr_output_state_set_render_format(&state,DRM_FORMAT_ARGB8888);
  wlr_output_state_set_custom_mode(&state,w->width,w->height,60000);
  bool ok=wlr_output_commit_state(w->output,&state);wlr_output_state_finish(&state);if(!ok){
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Window %d: initial ARGB output commit failed (%dx%d)",w->id,w->width,w->height);
    destroy_output(w);return;
  }
  __android_log_print(ANDROID_LOG_INFO,"MatonCompositor","Window %d: ARGB output ready (%dx%d)",w->id,w->width,w->height);
  w->scene_output=wlr_scene_output_create(w->scene,w->output);if(!w->scene_output){destroy_output(w);return;}
  w->presenter=maton_presenter_create(w->surface.control,server.allocator,&server.uploader);
  if(!w->presenter){destroy_output(w);return;}
  w->frame.notify=on_frame;wl_signal_add(&w->output->events.frame,&w->frame);
  w->commit.notify=on_commit;wl_signal_add(&w->output->events.commit,&w->commit);wlr_output_schedule_frame(w->output);
}
static void process_commands(void) {
  uint64_t value;while(read(server.event_fd,&value,sizeof(value))>0){}
  pthread_mutex_lock(&server.mutex);struct Command* list=server.first;server.first=server.last=NULL;pthread_mutex_unlock(&server.mutex);
  while(list){struct Command* c=list;list=c->next;struct Window* w=find_window(c->id);
    switch(c->kind){
    case CMD_ADD_SESSION:
      if(wl_display_add_socket_fd(server.display,c->session->fd)==0){
        c->session->next=server.sessions;server.sessions=c->session;
      }else{
        __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Session %d: cannot register Wayland socket",c->session->id);
        close(c->session->fd);unlink(c->session->path);free(c->session);
      }
      break;
    case CMD_ADD_XWAYLAND:handle_add_xwayland(c->a,(uid_t)c->b);break;
    case CMD_ATTACH:monitor_cover(c->a,c->b);attach_output(c);break;
    case CMD_DETACH:if(w){w->activity=0;destroy_output(w);}break;
    case CMD_RESIZE:if(c->a>0&&c->b>0)monitor_cover(c->a,c->b);if(w&&c->a>0&&c->b>0){w->width=c->a;w->height=c->b;if(w->output){struct wlr_output_state s;wlr_output_state_init(&s);wlr_output_state_set_custom_mode(&s,c->a,c->b,60000);bool committed=wlr_output_commit_state(w->output,&s);wlr_output_state_finish(&s);if(committed){if(w->toplevel&&w->toplevel->base->initialized)wlr_xdg_toplevel_set_size(w->toplevel,c->a,c->b);else if(w->xsurface)wlr_xwayland_surface_configure(w->xsurface,0,0,c->a,c->b);wlr_output_schedule_frame(w->output);}}}break;
    case CMD_KEY:if(server.seat&&server.keyboard_initialized){struct wlr_surface* focus=NULL;if(w&&w->toplevel&&w->toplevel->base->surface)focus=w->toplevel->base->surface;else if(w&&w->xsurface&&w->xsurface->surface)focus=w->xsurface->surface;if(focus&&server.seat->keyboard_state.focused_surface!=focus){wlr_seat_keyboard_notify_enter(server.seat,focus,NULL,0,&server.keyboard.modifiers);xwindow_activate(w);}struct wlr_keyboard_key_event e={.time_msec=(uint32_t)(c->time/1000000),.keycode=(uint32_t)(c->b>0?c->b:c->a+8),.update_state=true,.state=c->c==0?WL_KEYBOARD_KEY_STATE_PRESSED:WL_KEYBOARD_KEY_STATE_RELEASED};wlr_keyboard_notify_key(&server.keyboard,&e);wlr_seat_keyboard_notify_key(server.seat,e.time_msec,e.keycode,e.state);wlr_seat_keyboard_notify_modifiers(server.seat,&server.keyboard.modifiers);}break;
    case CMD_MOTION:if(server.seat&&w){if(c->a==10||c->a==3){clear_window_pointer(w);break;}uint32_t tm=(uint32_t)(c->time/1000000);double sx=0,sy=0;struct wlr_scene* scene=w?w->scene:NULL;struct wlr_scene_node* node=scene?wlr_scene_node_at(&scene->tree.node,c->x,c->y,&sx,&sy):NULL;struct wlr_scene_buffer* sb=node&&node->type==WLR_SCENE_NODE_BUFFER?wlr_scene_buffer_from_node(node):NULL;struct wlr_scene_surface* ss=sb?wlr_scene_surface_try_from_buffer(sb):NULL;if(ss){server.pointer_window=w->id;/* Compare with the seat's real focus, not a cache: a destroyed surface
       * or a popup grab can clear or move it, and coordinates for the surface
       * under the pointer would then reach another one. */
      if(server.seat->pointer_state.focused_surface!=ss->surface){server.pointer_focus=ss->surface;wlr_seat_pointer_notify_enter(server.seat,ss->surface,sx,sy);struct wlr_surface* root=window_root_surface(w);if(root&&server.seat->keyboard_state.focused_surface!=root){wlr_seat_keyboard_notify_enter(server.seat,root,NULL,0,&server.keyboard.modifiers);xwindow_activate(w);}}wlr_seat_pointer_notify_motion(server.seat,tm,sx,sy);}else{server.pointer_focus=NULL;wlr_seat_pointer_notify_clear_focus(server.seat);}struct PointerButtonEvent button_event={.time=tm,.window=w->id};maton_pointer_buttons_update(&server.pointer_buttons,ss ? (uint32_t)c->b : 0,notify_pointer_button,&button_event);/* Android reports wheel notches as +-1.0 (fractions on touchpads); the
       * discrete argument is value120, 120 per notch. */
      if(c->vs)wlr_seat_pointer_notify_axis(server.seat,tm,WL_POINTER_AXIS_VERTICAL_SCROLL,-c->vs*15.0,(int32_t)lroundf(-c->vs*120.0f),WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);if(c->hs)wlr_seat_pointer_notify_axis(server.seat,tm,WL_POINTER_AXIS_HORIZONTAL_SCROLL,-c->hs*15.0,(int32_t)lroundf(-c->hs*120.0f),WL_POINTER_AXIS_SOURCE_WHEEL,WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);wlr_seat_pointer_notify_frame(server.seat);}break;
    case CMD_RELEASE:if(c->buffer)wlr_buffer_unlock(c->buffer);break;
    case CMD_CLOSE:if(w&&w->toplevel){clear_window_pointer(w);wlr_xdg_toplevel_send_close(w->toplevel);}else if(w&&w->xsurface){clear_window_pointer(w);wlr_xwayland_surface_close(w->xsurface);}break;
    case CMD_STOP:atomic_store(&server.stopping,true);break;
    }free(c);
  }
}
static void handle_add_xwayland(int session,uid_t uid) {
  pthread_mutex_lock(&server.mutex);
  server.xw_ok=false;server.xw_display[0]=0;
  if(atomic_load(&server.started)&&!atomic_load(&server.stopping)&&server.xwayland_path[0]){
    struct XwaylandSession* xw=calloc(1,sizeof(*xw));
    if(xw){
      xw->session=session;xw->uid=uid;
      xw->xwayland=wlr_xwayland_create(server.display,server.compositor,true);
      if(xw->xwayland){
        wlr_xwayland_set_seat(xw->xwayland,server.seat);
        xw->ready.notify=on_xwayland_ready;wl_signal_add(&xw->xwayland->events.ready,&xw->ready);
        xw->new_surface.notify=on_xwayland_new_surface;wl_signal_add(&xw->xwayland->events.new_surface,&xw->new_surface);
        xw->destroy.notify=on_xwayland_destroy;wl_signal_add(&xw->xwayland->events.destroy,&xw->destroy);
        xw->next=server.xwayland_sessions;server.xwayland_sessions=xw;
        xwayland_apply_socket_mode(xw);
        snprintf(server.xw_display,sizeof(server.xw_display),"X%s",
            xw->xwayland->display_name+1);
        server.xw_ok=true;
      }else{free(xw);}
    }
  }
  if(!server.xw_ok)
    __android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Session %d: Xwayland creation failed",session);
  server.xw_completed++;
  pthread_cond_broadcast(&server.xw_cond);
  pthread_mutex_unlock(&server.mutex);
}
static int on_event_fd(int fd,uint32_t mask,void* data){(void)fd;(void)mask;(void)data;process_commands();return 0;}
/* GPU clients hand over dma-bufs that SurfaceFlinger shows as they are
 * (presenter.c, dmabuf_import.c). Only what gralloc can import is offered:
 * single-plane RGB formats, linear or the driver's implicit layout, on the
 * render node SurfaceFlinger's GPU uses. */
/* Every session has its own Xwayland and so its own xwayland_shell_v1 global,
 * which admits only that Xwayland. Advertising all of them lets one Xwayland
 * bind another's and fail ("Permission denied to bind to xwayland_shell_v1"),
 * so each is shown to its own Xwayland only (as wlroots' xwayland.h advises). */
static bool global_filter(const struct wl_client* client,const struct wl_global* global,void* data) {
  (void)data;
  for(struct XwaylandSession* xw=server.xwayland_sessions;xw;xw=xw->next){
    struct wlr_xwayland* x=xw->xwayland;
    if(!x||!x->shell_v1||x->shell_v1->global!=global)continue;
    return x->server&&x->server->client==client;
  }
  return true;
}
static bool render_node(dev_t* out) {
  DIR* dir=opendir("/dev/dri");if(!dir)return false;
  struct dirent* e;bool found=false;
  while(!found&&(e=readdir(dir))){
    if(strncmp(e->d_name,"renderD",7))continue;
    char path[64];snprintf(path,sizeof(path),"/dev/dri/%s",e->d_name);
    struct stat st;if(!stat(path,&st)&&S_ISCHR(st.st_mode)){*out=st.st_rdev;found=true;}
  }
  closedir(dir);return found;
}
static bool check_dmabuf(struct wlr_dmabuf_attributes* attrs,void* data) {
  (void)data;
  return attrs->n_planes==1&&maton_dmabuf_format_supported(attrs->format);
}
static bool create_linux_dmabuf(void) {
  static const uint32_t formats[]={DRM_FORMAT_ARGB8888,DRM_FORMAT_XRGB8888,DRM_FORMAT_ABGR8888,DRM_FORMAT_XBGR8888,DRM_FORMAT_RGB565};
  for(size_t i=0;i<sizeof(formats)/sizeof(formats[0]);++i)
    if(!wlr_drm_format_set_add(&server.dmabuf_formats,formats[i],DRM_FORMAT_MOD_LINEAR)||
        !wlr_drm_format_set_add(&server.dmabuf_formats,formats[i],DRM_FORMAT_MOD_INVALID))return false;
  dev_t device;
  if(!render_node(&device)){
    /* No GPU: clients stay on shared memory. */
    __android_log_print(ANDROID_LOG_WARN,"MatonCompositor","No DRM render node; linux-dmabuf disabled");
    return true;
  }
  struct wlr_linux_dmabuf_feedback_v1 feedback={.main_device=device};
  wl_array_init(&feedback.tranches);
  struct wlr_linux_dmabuf_feedback_v1_tranche* tranche=wlr_linux_dmabuf_feedback_add_tranche(&feedback);
  struct wlr_drm_format_set none={0};
  bool ok=tranche&&wlr_drm_format_set_union(&tranche->formats,&none,&server.dmabuf_formats);
  if(ok)tranche->target_device=device;
  struct wlr_linux_dmabuf_v1* dmabuf=ok?wlr_linux_dmabuf_v1_create(server.display,4,&feedback):NULL;
  wlr_linux_dmabuf_feedback_v1_finish(&feedback);
  if(!dmabuf)return false;
  wlr_linux_dmabuf_v1_set_check_dmabuf_callback(dmabuf,check_dmabuf,NULL);
  return true;
}
static void* server_main(void* unused) {
  (void)unused;bool initialized=false;const char* stage="display";
  wlr_log_init(WLR_ERROR,maton_wlr_log);
  server.display=wl_display_create();
  if(!server.display)goto done;server.loop=wl_display_get_event_loop(server.display);
  maton_surface_output_set_release_dispatch(release_buffer);
  stage="event loop";server.event_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
  if(server.event_fd<0||!wl_event_loop_add_fd(server.loop,server.event_fd,WL_EVENT_READABLE,on_event_fd,NULL))goto done;
  stage="backend and renderer";server.backend=wlr_headless_backend_create(server.loop);server.renderer=maton_renderer_create(wlr_pixman_renderer_create(),&server.dmabuf_formats);
  if(!server.backend||!server.renderer||!maton_ahb_allocator_init(&server.ahb_allocator))goto done;
  stage="renderer globals";if(!wlr_renderer_init_wl_display(server.renderer,server.display))goto done;
  stage="linux-dmabuf";if(!create_linux_dmabuf())goto done;
  wl_display_set_global_filter(server.display,global_filter,NULL);
  server.allocator=maton_ahb_allocator_base(&server.ahb_allocator);
  if(!maton_egl_uploader_init(&server.uploader))__android_log_print(ANDROID_LOG_WARN,"MatonCompositor","EGL upload fallback unavailable");
  wlr_subcompositor_create(server.display);wlr_viewporter_create(server.display);
  if(!wlr_data_device_manager_create(server.display))goto done;
  server.xdg_shell=wlr_xdg_shell_create(server.display,6);server.compositor=wlr_compositor_create(server.display,6,server.renderer);server.seat=wlr_seat_create(server.display,"seat0");
  stage="shell and seat";if(!server.xdg_shell||!server.compositor||!server.seat)goto done;
  /* Server-side-only window decorations: Android's caption bar decorates. */
  stage="decorations";
  struct wlr_server_decoration_manager* legacy_decoration=wlr_server_decoration_manager_create(server.display);
  struct wlr_xdg_decoration_manager_v1* decorations=wlr_xdg_decoration_manager_v1_create(server.display);
  if(!legacy_decoration||!decorations)goto done;
  wlr_server_decoration_manager_set_default_mode(legacy_decoration,WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
  server.new_decoration.notify=on_new_decoration;wl_signal_add(&decorations->events.new_toplevel_decoration,&server.new_decoration);
  wlr_keyboard_init(&server.keyboard,NULL,"maton-keyboard");server.keyboard_initialized=1;
  struct xkb_context* xc=xkb_context_new(XKB_CONTEXT_NO_FLAGS);struct xkb_keymap* km=xc?xkb_keymap_new_from_string(xc,kMatonUsKeymap,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS):NULL;
  if(km){(void)wlr_keyboard_set_keymap(&server.keyboard,km);xkb_keymap_unref(km);}if(xc)xkb_context_unref(xc);
  wlr_seat_set_keyboard(server.seat,&server.keyboard);wlr_seat_set_capabilities(server.seat,WL_SEAT_CAPABILITY_KEYBOARD|WL_SEAT_CAPABILITY_POINTER);
  server.new_toplevel.notify=on_new_toplevel;wl_signal_add(&server.xdg_shell->events.new_toplevel,&server.new_toplevel);
  stage="virtual monitor";struct wlr_output* monitor=wlr_headless_add_output(server.backend,1280,720);
  /* This output advertises geometry only; Android window outputs render.
   * Leaving its renderer unset avoids allocating an unused scanout buffer. */
  if(!monitor)goto done;
  wlr_output_set_name(monitor,"MatonOS");
  struct wlr_output_state monitor_state;wlr_output_state_init(&monitor_state);
  wlr_output_state_set_enabled(&monitor_state,true);wlr_output_state_set_custom_mode(&monitor_state,1280,720,60000);
  bool monitor_ok=wlr_output_commit_state(monitor,&monitor_state);wlr_output_state_finish(&monitor_state);
  if(!monitor_ok)goto done;
  server.monitor=monitor;
  /* SDL's Wayland backend needs logical monitor geometry for scaling. The
   * layout owns the wl_output global and is destroyed with the display. */
  stage="logical monitor";struct wlr_output_layout* layout=wlr_output_layout_create(server.display);
  if(!layout||!wlr_output_layout_add(layout,monitor,0,0)||
      !wlr_xdg_output_manager_v1_create(server.display,layout))goto done;
  server.new_popup.notify=on_new_popup;wl_signal_add(&server.xdg_shell->events.new_popup,&server.new_popup);
  stage="Wayland socket";if(wl_display_add_socket(server.display,server.socket_name)<0)goto done;
  stage="backend start";if(!wlr_backend_start(server.backend))goto done;
  char socket_path[640];snprintf(socket_path,sizeof(socket_path),"%s/%s",server.runtime_dir,server.socket_name);
  stage="socket permissions";if(chmod(socket_path,0666))goto done;
  initialized=true;
done:
  if(!initialized)__android_log_print(ANDROID_LOG_ERROR,"MatonCompositor","Startup failed at %s (errno=%d)",stage,errno);
  pthread_mutex_lock(&server.mutex);server.ok=initialized;server.ready=1;pthread_cond_broadcast(&server.ready_cond);pthread_mutex_unlock(&server.mutex);
  if(server.ok){while(!atomic_load(&server.stopping)){wl_display_flush_clients(server.display);wl_event_loop_dispatch(server.loop,-1);}wl_display_destroy_clients(server.display);}
  while(server.xwayland_sessions){struct XwaylandSession* xw=server.xwayland_sessions;server.xwayland_sessions=xw->next;xw->next=NULL;if(xw->xwayland){/* The destroy listener unlinks and frees the session. */wlr_xwayland_destroy(xw->xwayland);}else{free(xw);}}
  while(server.windows)destroy_window(server.windows);
  if(server.new_popup.link.prev){wl_list_remove(&server.new_popup.link);wl_list_init(&server.new_popup.link);}
  if(server.new_decoration.link.prev){wl_list_remove(&server.new_decoration.link);wl_list_init(&server.new_decoration.link);}
  if(server.new_toplevel.link.prev){wl_list_remove(&server.new_toplevel.link);wl_list_init(&server.new_toplevel.link);}
  if(server.keyboard_initialized)wlr_keyboard_finish(&server.keyboard);server.keyboard_initialized=0;
  if(server.seat)wlr_seat_destroy(server.seat);server.seat=NULL;server.pointer_buttons=0;server.pointer_focus=NULL;server.pointer_window=0;
  if(server.allocator)wlr_allocator_destroy(server.allocator);server.allocator=NULL;
  if(server.renderer)wlr_renderer_destroy(server.renderer);server.renderer=NULL;
  /* The renderer reports these as its dma-buf formats: free it after. */
  wlr_drm_format_set_finish(&server.dmabuf_formats);
  maton_egl_uploader_finish(&server.uploader);if(server.backend)wlr_backend_destroy(server.backend);server.backend=NULL;
  if(server.display)wl_display_destroy(server.display);server.display=NULL;server.loop=NULL;
  while(server.sessions){struct SocketSession* s=server.sessions;server.sessions=s->next;unlink(s->path);free(s);}
  if(server.event_fd>=0)close(server.event_fd);server.event_fd=-1;return NULL;
}
bool maton_core_start(const char* socket_name,const char* runtime_dir){
  bool expected=false;if(!atomic_compare_exchange_strong(&server.started,&expected,true)){pthread_mutex_lock(&server.mutex);bool ok=server.ok;pthread_mutex_unlock(&server.mutex);return ok;}
  if(!socket_name||!runtime_dir||strlen(socket_name)>=sizeof(server.socket_name)||strlen(runtime_dir)>=sizeof(server.runtime_dir))goto fail;
  strcpy(server.socket_name,socket_name);strcpy(server.runtime_dir,runtime_dir);
  if(mkdir(runtime_dir,0700)&&errno!=EEXIST)goto fail;if(chmod(runtime_dir,0711)||setenv("XDG_RUNTIME_DIR",runtime_dir,1))goto fail;
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
void maton_core_motion(int id,float x,float y,float vs,float hs,int action,int buttons,int64_t time){command(CMD_MOTION,id,action,buttons,0,time,x,y,vs,hs,NULL,NULL);}

void maton_core_close(int id){command(CMD_CLOSE,id,0,0,0,0,0,0,0,0,NULL,NULL);}

bool maton_core_xwayland_init(const char* socket_dir,const char* xwayland_path) {
  if(!socket_dir||!xwayland_path||strlen(socket_dir)>=sizeof(server.xwayland_dir)||
      strlen(xwayland_path)>=sizeof(server.xwayland_path))return false;
  strcpy(server.xwayland_dir,socket_dir);
  strcpy(server.xwayland_path,xwayland_path);
  /* Only consulted from the Wayland thread; Xwayland sessions are created
   * after the server starts, so the environment is in place in time. */
  if(setenv("WLR_XWAYLAND_SOCKET_DIR",socket_dir,1))return false;
  /* wlroots bakes the build-host Xwayland path from xwayland.pc into its
   * binary check and exec; WLR_XWAYLAND overrides both at runtime. */
  char binary[512];
  int written=snprintf(binary,sizeof(binary),"%s/Xwayland",xwayland_path);
  if(written<0||written>=(int)sizeof(binary))return false;
  /* Xwayland admits only its own UID, but X clients arrive through linuxd's
   * relay as another user. The socket lives in this app's private directory
   * and reaches linuxd only as a delegated fd, so that is the access gate. */
  /* Xwayland's glamor uses the EGL/GLES/GBM forwarders in <prefix>/lib64/xwayland
   * (linux/compositor/xwayland-egl); only Xwayland, which inherits this
   * environment, loads them. */
  char libraries[512];
  size_t prefix=strlen(xwayland_path);
  if(prefix>=4&&!strcmp(xwayland_path+prefix-4,"/bin"))prefix-=4;
  written=snprintf(libraries,sizeof(libraries),"%.*s/lib64/xwayland",(int)prefix,xwayland_path);
  if(written<0||written>=(int)sizeof(libraries)||setenv("LD_LIBRARY_PATH",libraries,1))return false;
  return setenv("WLR_XWAYLAND",binary,1)==0&&
      setenv("WLR_XWAYLAND_NO_ABSTRACT","1",1)==0&&
      setenv("WLR_XWAYLAND_NO_ACCESS_CONTROL","1",1)==0;
}
bool maton_core_add_xwayland(int session,int uid,char* display,size_t display_size) {
  if(session<=0||!display||display_size==0)return false;
  pthread_mutex_lock(&server.mutex);
  if(!atomic_load(&server.started)||atomic_load(&server.stopping)||!server.xwayland_path[0]){
    pthread_mutex_unlock(&server.mutex);return false;
  }
  unsigned sent=++server.xw_sent;
  pthread_mutex_unlock(&server.mutex);
  struct Command* c=calloc(1,sizeof(*c));
  if(!c)return false;
  c->kind=CMD_ADD_XWAYLAND;c->a=session;c->b=uid;enqueue(c);
  pthread_mutex_lock(&server.mutex);
  struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);limit.tv_sec+=5;
  while(server.xw_completed<sent){
    if(pthread_cond_timedwait(&server.xw_cond,&server.mutex,&limit))break;
  }
  bool ok=server.xw_completed>=sent&&server.xw_ok;
  if(ok)snprintf(display,display_size,"%s",server.xw_display);
  pthread_mutex_unlock(&server.mutex);
  return ok;
}
bool maton_core_add_session(int id,const char* path) {
  if(id<=0||!path||strlen(path)>=sizeof(((struct SocketSession*)0)->path)||
      !atomic_load(&server.started)||atomic_load(&server.stopping))return false;
  struct SocketSession* s=calloc(1,sizeof(*s));struct Command* c=calloc(1,sizeof(*c));
  if(!s||!c){free(s);free(c);return false;}
  s->id=id;strcpy(s->path,path);
  s->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
  struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
  unlink(path);
  if(s->fd<0||bind(s->fd,(struct sockaddr*)&address,sizeof(address))||chmod(path,0666)||listen(s->fd,128)){
    if(s->fd>=0)close(s->fd);unlink(path);free(s);free(c);return false;
  }
  /* Accept registration runs on the Wayland thread. Connections arriving
   * before it runs wait in this listening socket's backlog. */
  c->kind=CMD_ADD_SESSION;c->session=s;enqueue(c);return true;
}
