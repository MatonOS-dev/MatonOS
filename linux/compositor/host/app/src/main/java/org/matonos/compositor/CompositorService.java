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
    private static native boolean nativeStart(String socketName, String runtimeDir, CompositorService owner);
    private static native void nativeStop();
    private static native void nativeLaunchDemo();
    private static native void nativeAttach(int id, android.view.Surface surface, int width, int height);
    private static native void nativeDetach(int id);
    private static native void nativeKey(int id, int key, int scan, int action, int meta, long timeNanos);
    private static native void nativeMotion(int id, float x, float y, float verticalScroll, float horizontalScroll, int action, int buttons, long timeNanos);
    private static native void nativeResize(int id, int width, int height);

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
        if (!nativeStart("wayland-0", getFilesDir().getAbsolutePath() + "/wayland", this)) Log.e(TAG, "Compositor failed to start; Android service remains responsive");
    }

    public void onNativeToplevel(int windowId, int width, int height) {
        Intent window = new Intent(this, WindowActivity.class)
                .putExtra(WindowActivity.EXTRA_WINDOW_ID, windowId)
                .putExtra(WindowActivity.EXTRA_WIDTH, width)
                .putExtra(WindowActivity.EXTRA_HEIGHT, height)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(window);
    }

    public void onNativeToplevelClosed(int windowId) {
        Intent close = new Intent(WindowActivity.ACTION_CLOSE_WINDOW)
                .setPackage(getPackageName()).putExtra(WindowActivity.EXTRA_WINDOW_ID, windowId);
        sendBroadcast(close);
    }

    private final ICompositor.Stub binder = new ICompositor.Stub() {
        public void launchDemo() { nativeLaunchDemo(); }
        public void attachWindow(int id, android.view.Surface s, int w, int h) { nativeAttach(id, s, w, h); }
        public void detachWindow(int id) { nativeDetach(id); }
        public void keyEvent(int id, int key, int scan, int action, int meta, long time) { nativeKey(id, key, scan, action, meta, time); }
        public void motionEvent(int id, float x, float y, float vs, float hs, int action, int buttons, long time) { nativeMotion(id, x, y, vs, hs, action, buttons, time); }
        public void resizeWindow(int id, int w, int h) { nativeResize(id, w, h); }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public void onDestroy() { nativeStop(); super.onDestroy(); }
}
