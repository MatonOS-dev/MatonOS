package org.matonos.compositor;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/** Coordinates verified launches and DNS; holds no compositor or display sockets. */
public final class CompositorService extends Service {
    private static final String TAG = "MatonCompositor";
    private static final String CHANNEL = "wayland_compositor";
    private static final int NOTIFICATION_ID = 1001;
    private final java.util.concurrent.ConcurrentHashMap<String, EmbeddedSession> sessions = new java.util.concurrent.ConcurrentHashMap<>();

    @Override public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Linux applications", NotificationManager.IMPORTANCE_LOW));
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.ic_menu_view)
                .setContentTitle("Linux application support").setContentIntent(pi).setOngoing(true).build();
        startForeground(NOTIFICATION_ID, n);
    }

    private final IEmbeddedHost.Stub embedded = new IEmbeddedHost.Stub() {
        public IEmbeddedSession openSession(String ref, IEmbeddedWindowListener listener, android.os.ParcelFileDescriptor lifeline, String dnsForwarder) {
            int pid = android.os.Binder.getCallingPid();
            int uid = android.os.Binder.getCallingUid();
            if (lifeline == null || listener == null || ref == null || !ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
                throw new SecurityException("Invalid stub session");
            long identity = android.os.Binder.clearCallingIdentity();
            try {
                if (!FlatpakLauncher.verifyStub(CompositorService.this, uid, ref))
                    throw new SecurityException("Caller is not the signed launcher for this application");
                synchronized (sessions) {
                    EmbeddedSession session = sessions.get(ref);
                    if (session != null && session.pid != pid) {
                        // A new process cannot adopt an old process's lifetime.
                        if (session.uid != uid) throw new SecurityException("Session belongs to another UID");
                        android.system.StructPollfd life = new android.system.StructPollfd();
                        life.fd = session.lifeline.getFileDescriptor();
                        life.events = (short)android.system.OsConstants.POLLIN;
                        android.system.Os.poll(new android.system.StructPollfd[]{life}, 0);
                        if ((life.revents & android.system.OsConstants.POLLHUP) == 0)
                            throw new IllegalStateException("Previous app process is still stopping");
                        session.closeResources();
                        sessions.remove(ref);session=null;
                    }
                    if (session == null) {
                        if (sessions.size() >= 128) throw new IllegalStateException("Too many application sessions");
                        session = new EmbeddedSession(uid, ref, dnsForwarder);
                        session.pid=pid; session.lifeline=lifeline;
                        sessions.put(ref, session);
                        session.watchLifeline();
                    }
                    if (session.pid != pid) throw new SecurityException("Previous stub session is still registered; restart the host session");
                    if (session.lifeline != lifeline) lifeline.close();
                    if (session.uid != uid) throw new SecurityException("Session belongs to another UID");
                    session.listeners.register(listener);
                    listener.onInhibitChanged(false);
                    return session;
                }
            } catch (SecurityException e) { throw e; }
            catch (Exception e) { throw new IllegalStateException("Cannot open application session", e); }
            finally { android.os.Binder.restoreCallingIdentity(identity); }
        }
    };

    private final class EmbeddedSession extends IEmbeddedSession.Stub {
        final int uid;
        int pid;
        android.os.ParcelFileDescriptor lifeline;
        final String ref;
        final String dnsForwarder;
        final android.os.RemoteCallbackList<IEmbeddedWindowListener> listeners = new android.os.RemoteCallbackList<>();
        EmbeddedSession(int uid, String ref, String dnsForwarder) throws Exception {
            this.uid=uid;this.ref=ref;
            this.dnsForwarder=dnsForwarder;
            // The app's Wayland session is created by its own in-process core
            // (PerAppRuntime); the host no longer creates a session for it.
        }
        void watchLifeline() {
            if (lifeline == null) return;
            final android.os.ParcelFileDescriptor watch;
            try { watch = android.os.ParcelFileDescriptor.dup(lifeline.getFileDescriptor()); }
            catch (Exception error) { Log.e(TAG, "Cannot monitor stub process lifetime", error); return; }
            new Thread(() -> {
                try (java.io.InputStream stream = new android.os.ParcelFileDescriptor.AutoCloseInputStream(watch)) {
                    while (stream.read() >= 0) { /* The pipe carries no data; EOF means stub process exit. */ }
                } catch (Exception ignored) { }
                synchronized (sessions) {
                    if (sessions.get(ref) != this) return;
                    sessions.remove(ref, this);
                }
                closeResources();
            }, "stub-lifeline-" + uid).start();
        }
        void closeResources() {
            if (lifeline != null) try { lifeline.close(); } catch (Exception ignored) {}
        }
        private void check() {
            if (android.os.Binder.getCallingUid()!=uid) throw new SecurityException("Session belongs to another UID");
        }
        public synchronized String launch() {
            check();
            long identity=android.os.Binder.clearCallingIdentity();
            try {
                String reply=FlatpakLauncher.launch(CompositorService.this,ref,dnsForwarder,uid,pid,lifeline);
                return reply;
            } catch(Exception e) { return failure(e); }
            finally { android.os.Binder.restoreCallingIdentity(identity); }
        }
        public void unregisterListener(IEmbeddedWindowListener listener){check();listeners.unregister(listener);}

    }
    private static String failure(Exception error) {
        Log.e(TAG,"Embedded application request failed",error);
        try {return new org.json.JSONObject().put("ok",false).put("error",error.getMessage()!=null?error.getMessage():error.getClass().getSimpleName()).toString();}
        catch(Exception ignored){return "{\"ok\":false,\"error\":\"Application request failed\"}";}
    }
    @Override public IBinder onBind(Intent intent) {
        return "org.matonos.compositor.EMBEDDED".equals(intent.getAction()) ? embedded : null;
    }
    @Override public void onDestroy() {
        for(EmbeddedSession session:sessions.values()) {session.closeResources();}
        super.onDestroy();
    }
}
