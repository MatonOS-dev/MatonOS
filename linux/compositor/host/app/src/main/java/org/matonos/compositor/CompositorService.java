package org.matonos.compositor;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

public final class CompositorService extends Service {
    private static final String TAG = "MatonCompositor";
    private static final String CHANNEL = "wayland_compositor";
    private static final int NOTIFICATION_ID = 1001;
    static final String ACTION_WINDOW_OPENED = "org.matonos.compositor.WINDOW_OPENED";
    private static native boolean nativeStart(String socketName, String runtimeDir, CompositorService owner);
    private static native void nativeStop();
    private static native boolean nativeAddSession(int id, String path);
    private static native boolean nativeXwaylandInit(String socketDir, String xwaylandPath);
    private static native void nativeStopped(int window, boolean stopped);
    private static native String nativeAddXwayland(int session, int uid);
    private static native void nativeClose(int id);
    private static native void nativeLaunchDemo();
    private static native void nativeAttach(int id, android.view.Surface surface, int width, int height);
    private static native void nativeDetach(int id);
    private static native void nativeKey(int id, int key, int scan, int action, int meta, long timeNanos);
    private static native void nativeMotion(int id, float x, float y, float verticalScroll, float horizontalScroll, int action, int buttons, long timeNanos);
    private static native void nativeResize(int id, int width, int height);

    private volatile boolean ready;
    static final String ACTION_INHIBIT = "org.matonos.compositor.INHIBIT";
    private final java.util.concurrent.ConcurrentHashMap<String, Long> launching = new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.concurrent.ConcurrentHashMap<String, EmbeddedSession> sessions = new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.concurrent.ConcurrentHashMap<Integer, EmbeddedSession> sessionsById = new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.concurrent.atomic.AtomicInteger nextSession = new java.util.concurrent.atomic.AtomicInteger(1);
    static { System.loadLibrary("maton_compositor"); }

    @Override public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Wayland compositor", NotificationManager.IMPORTANCE_LOW));
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.ic_menu_view)
                .setContentTitle("Wayland compositor running").setContentIntent(pi).setOngoing(true).build();
        startForeground(NOTIFICATION_ID, n);
        ready = nativeStart("wayland-0", getFilesDir().getAbsolutePath() + "/wayland", this);
        if (ready) {
            // The advertised monitor is what clients (GTK, Firefox, Xwayland's
            // root window) take as the screen; window id 0 resizes only it.
            android.graphics.Rect display = getSystemService(android.view.WindowManager.class)
                    .getMaximumWindowMetrics().getBounds();
            nativeResize(0, display.width(), display.height());
            // Per-app Xwayland sockets stay in a private directory delegated
            // to linuxd through Binder;
            // lazy start keeps the cost at zero for Wayland-only apps.
            // Fail closed when the Xwayland server binary is not shipped.
            java.io.File x11dir = new java.io.File(getFilesDir(),"x11");
            if (!new java.io.File("/system_ext/bin/Xwayland").isFile()) {
                Log.i(TAG, "Xwayland server not installed; X11 launch disabled");
            } else {
                try {
                    if (!x11dir.isDirectory() && !x11dir.mkdirs())
                        throw new java.io.IOException("Cannot create X11 socket directory");
                    android.system.Os.chmod(x11dir.getAbsolutePath(),0711);
                    if (!nativeXwaylandInit(x11dir.getAbsolutePath(), "/system_ext/bin"))
                        Log.w(TAG, "Xwayland environment setup failed; Xwayland disabled");
                } catch (Exception error) { Log.w(TAG, "X11 socket directory unavailable; Xwayland disabled", error); }
            }
        }
        if (!ready) Log.e(TAG, "Compositor failed to start; Android service remains responsive");
    }

    public void onNativeToplevel(int sessionId, int windowId, int width, int height) {
        if (sessionId != 0) {
            EmbeddedSession session = sessionsById.get(sessionId);
            if (session != null) session.opened(windowId, width, height);
            else nativeClose(windowId);
            return;
        }
        Intent window = new Intent(this, WindowActivity.class)
                .putExtra(WindowActivity.EXTRA_WINDOW_ID, windowId)
                .putExtra(WindowActivity.EXTRA_WIDTH, width)
                .putExtra(WindowActivity.EXTRA_HEIGHT, height)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
        startActivity(window);
        sendBroadcast(new Intent(ACTION_WINDOW_OPENED).setPackage(getPackageName()));
    }

    public void onNativeToplevelClosed(int windowId) {
        for (EmbeddedSession session : sessions.values()) {
            if (session.closed(windowId)) return;
        }
        Intent close = new Intent(WindowActivity.ACTION_CLOSE_WINDOW)
                .setPackage(getPackageName()).putExtra(WindowActivity.EXTRA_WINDOW_ID, windowId);
        sendBroadcast(close);
    }

    private boolean sessionInhibited() {
        for(EmbeddedSession session:sessions.values())if(session.bus.portals.isHeld())return true;
        return false;
    }

    private final ICompositor.Stub binder = new ICompositor.Stub() {
        @Override public boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
                throws android.os.RemoteException {
            if (android.os.Binder.getCallingUid() != android.os.Process.myUid())
                throw new SecurityException("Host-only compositor interface");
            return super.onTransact(code, data, reply, flags);
        }
        public String launchFlatpak(String ref) {
            return "{\"ok\":false,\"error\":\"Launch from the application stub\"}";
        }
        public String getLaunchStatus(String ref) {
            try {return FlatpakLauncher.status(CompositorService.this,ref);}
            catch(Exception error) {
                try {return new org.json.JSONObject().put("ok",false).put("error",error.getMessage()).toString();}
                catch(Exception ignored){return "{\"ok\":false}";}
            }
        }
        public void closeWindow(int id) { nativeClose(id); }
        public boolean isInhibited() { return sessionInhibited(); }
        public void launchDemo() { nativeLaunchDemo(); }
        public void attachWindow(int id, android.view.Surface s, int w, int h) { nativeAttach(id, s, w, h); }
        public void detachWindow(int id) { nativeDetach(id); }
        public void keyEvent(int id, int key, int scan, int action, int meta, long time) { nativeKey(id, key, scan, action, meta, time); }
        public void motionEvent(int id, float x, float y, float vs, float hs, int action, int buttons, long time) { nativeMotion(id, x, y, vs, hs, action, buttons, time); }
        public void resizeWindow(int id, int w, int h) { nativeResize(id, w, h); }
    };

    private final IEmbeddedHost.Stub embedded = new IEmbeddedHost.Stub() {
        public IEmbeddedSession openSession(String ref, IEmbeddedWindowListener listener, android.os.ParcelFileDescriptor lifeline) {
            int pid = android.os.Binder.getCallingPid();
            int uid = android.os.Binder.getCallingUid();
            if (lifeline == null || listener == null || ref == null || !ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
                throw new SecurityException("Invalid stub session");
            long identity = android.os.Binder.clearCallingIdentity();
            try {
                if (!FlatpakLauncher.verifyStub(CompositorService.this, uid, ref))
                    throw new SecurityException("Caller is not the signed launcher for this application");
                if (!ready) throw new IllegalStateException("Compositor failed to start");
                synchronized (sessions) {
                    EmbeddedSession session = sessions.get(ref);
                    if (session != null && session.pid != pid) {
                        // A new process cannot adopt an old process's lifetime.
                        String status=FlatpakLauncher.status(CompositorService.this,ref);
                        if(new org.json.JSONObject(status).optBoolean("ok"))throw new IllegalStateException("Previous app process is still stopping");
                        session.bus.close();if(session.lifeline!=null)session.lifeline.close();
                        sessionsById.remove(session.id);sessions.remove(ref);session=null;
                    }
                    if (session == null) {
                        if (sessions.size() >= 128) throw new IllegalStateException("Too many application sessions");
                        session = new EmbeddedSession(uid, ref, nextSession.getAndIncrement());
                        session.pid=pid; session.lifeline=lifeline;
                        sessionsById.put(session.id, session);
                        sessions.put(ref, session);
                    }
                    if (session.pid != pid) throw new SecurityException("Previous stub session is still registered; restart the host session");
                    if (session.lifeline != lifeline) lifeline.close();
                    if (session.uid != uid) throw new SecurityException("Session belongs to another UID");
                    session.listeners.register(listener);
                    listener.onInhibitChanged(session.bus.portals.isHeld());
                    session.ensureXwayland();
                    return session;
                }
            } catch (SecurityException e) { throw e; }
            catch (Exception e) { throw new IllegalStateException("Cannot open application session", e); }
            finally { android.os.Binder.restoreCallingIdentity(identity); }
        }
    };

    private final class EmbeddedSession extends IEmbeddedSession.Stub {
        final int uid, id;
        int pid;
        android.os.ParcelFileDescriptor lifeline;
        final String ref;
        final java.io.File directory;
        final java.util.concurrent.ConcurrentHashMap<Integer, int[]> windows = new java.util.concurrent.ConcurrentHashMap<>();
        final android.os.RemoteCallbackList<IEmbeddedWindowListener> listeners = new android.os.RemoteCallbackList<>();
        final SessionBus bus;
        boolean standalone;
        volatile String x11Display;
        boolean launched;
        EmbeddedSession(int uid, String ref, int id) throws Exception {
            this.uid=uid;this.ref=ref;this.id=id;
            directory=new java.io.File(getFilesDir(),"wayland/s"+id);
            if (!directory.isDirectory() && !directory.mkdirs()) throw new java.io.IOException("Cannot create application socket directory");
            android.system.Os.chmod(directory.getAbsolutePath(),0711);
            bus=new SessionBus(CompositorService.this,directory,ref,uid,this::inhibited,this::openUri);
            if (!nativeAddSession(id,new java.io.File(directory,"wayland-0").getAbsolutePath())) {
                bus.close();
                throw new java.io.IOException("Cannot create application Wayland socket");
            }
        }
        /** Best effort: the lazy Xwayland server starts only when the
         * application actually connects to its X11 display. */
        void ensureXwayland() {
            if (x11Display != null) return;
            try { x11Display = nativeAddXwayland(id, uid); }
            catch (Throwable error) { Log.w(TAG, "Xwayland unavailable for session " + id, error); }
        }
        private void check() {
            if (android.os.Binder.getCallingUid()!=uid) throw new SecurityException("Session belongs to another UID");
        }
        private void checkWindow(int window) {
            check();
            if (!windows.containsKey(window)) throw new SecurityException("Window belongs to another session");
        }
        boolean openUri(Intent intent) throws Exception {
            // URI grants must reach the verified stub before it launches the
            // handler from its own activity/task. Existing bridge checks still
            // authenticate the session; this callback has no privileged API.
            android.net.Uri grant=intent.getData();
            if(grant==null && intent.getClipData()!=null && intent.getClipData().getItemCount()>0)grant=intent.getClipData().getItemAt(0).getUri();
            if(grant!=null && "content".equals(grant.getScheme())) {
                String[] packages=getPackageManager().getPackagesForUid(uid);
                if(packages!=null)for(String pkg:packages)grantUriPermission(pkg,grant,intent.getFlags() &
                    (Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION));
            }
            synchronized(listeners) {
                int count=listeners.beginBroadcast();
                try {for(int i=count-1;i>=0;i--)try {if(listeners.getBroadcastItem(i).openUri(intent))return true;}catch(android.os.RemoteException ignored){} }
                finally {listeners.finishBroadcast();}
            }
            try {startActivity(new Intent(intent).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));return true;}
            catch(android.content.ActivityNotFoundException e){return false;}
        }
        void inhibited(boolean active) {
            if(standalone)sendBroadcast(new Intent(ACTION_INHIBIT).setPackage(getPackageName()).putExtra("active",sessionInhibited()));
            synchronized(listeners) {
                int count=listeners.beginBroadcast();
                try { for(int i=0;i<count;i++) try { listeners.getBroadcastItem(i).onInhibitChanged(active); } catch(android.os.RemoteException ignored){} }
                finally { listeners.finishBroadcast(); }
            }
        }
        void opened(int window,int width,int height) {
            windows.put(window,new int[]{width,height});
            synchronized (listeners) {
                int count=listeners.beginBroadcast();
                try {
                    if (count==0) {
                        if(standalone)onNativeToplevel(0,window,width,height);else nativeClose(window);
                        return;
                    }
                    listeners.getBroadcastItem(count-1).onWindowOpened(window,width,height);
                } catch (android.os.RemoteException e) { nativeClose(window); }
                finally { listeners.finishBroadcast(); }
            }
        }
        boolean closed(int window) {
            if (windows.remove(window)==null) return false;
            if(standalone)return false;
            synchronized (listeners) {
                int count=listeners.beginBroadcast();
                try { for(int i=0;i<count;i++) try { listeners.getBroadcastItem(i).onWindowClosed(window); } catch(android.os.RemoteException ignored){} }
                finally { listeners.finishBroadcast(); }
            }
            return true;
        }
        public synchronized String launch() {
            check();
            long identity=android.os.Binder.clearCallingIdentity();
            try {
                if (launched && !windows.isEmpty()) {
                    java.util.Map.Entry<Integer,int[]> window=windows.entrySet().iterator().next();
                    opened(window.getKey(),window.getValue()[0],window.getValue()[1]);
                    return "{\"ok\":true}";
                }
                if (launched) {
                    String status=FlatpakLauncher.status(CompositorService.this,ref);
                    if (new org.json.JSONObject(status).optBoolean("ok")) return status;
                }
                if(!bus.isAlive())throw new IllegalStateException("Session broker exited");
                String reply=FlatpakLauncher.launch(CompositorService.this,ref,directory,x11Display,uid,pid,lifeline);
                launched=new org.json.JSONObject(reply).optBoolean("ok");
                return reply;
            } catch(Exception e) { return failure(e); }
            finally { android.os.Binder.restoreCallingIdentity(identity); }
        }
        public String getLaunchStatus() {
            check();long identity=android.os.Binder.clearCallingIdentity();
            try { return FlatpakLauncher.status(CompositorService.this,ref); }
            catch(Exception e){return failure(e);}
            finally{android.os.Binder.restoreCallingIdentity(identity);}
        }
        public void unregisterListener(IEmbeddedWindowListener listener){check();listeners.unregister(listener);}
        public void attachWindow(int window,android.view.Surface surface,int width,int height){checkWindow(window);nativeAttach(window,surface,width,height);}
        public void detachWindow(int window){checkWindow(window);nativeDetach(window);}
        public void resizeWindow(int window,int width,int height){checkWindow(window);nativeResize(window,width,height);}
        public void closeWindow(int window){checkWindow(window);nativeClose(window);}
        public void setWindowStopped(int window,boolean stopped){checkWindow(window);nativeStopped(window,stopped);}
        public void keyEvent(int window,int key,int scan,int action,int meta,long time){checkWindow(window);nativeKey(window,key,scan,action,meta,time);}
        public void motionEvent(int window,float x,float y,float vs,float hs,int action,int buttons,long time){checkWindow(window);nativeMotion(window,x,y,vs,hs,action,buttons,time);}
    }
    private static String failure(Exception error) {
        Log.e(TAG,"Embedded application request failed",error);
        try {return new org.json.JSONObject().put("ok",false).put("error",error.getMessage()!=null?error.getMessage():error.getClass().getSimpleName()).toString();}
        catch(Exception ignored){return "{\"ok\":false,\"error\":\"Application request failed\"}";}
    }
    @Override public IBinder onBind(Intent intent) {
        return "org.matonos.compositor.EMBEDDED".equals(intent.getAction()) ? embedded : binder;
    }
    @Override public void onDestroy() {
        for(EmbeddedSession session:sessions.values()) {session.bus.close();}
        nativeStop(); super.onDestroy();
    }
}
