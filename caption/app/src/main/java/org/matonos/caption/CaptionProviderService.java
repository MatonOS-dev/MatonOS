package org.matonos.caption;

import android.app.Service;
import android.content.Intent;
import android.graphics.Rect;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.SurfaceControlViewHost;
import android.view.View;
import java.util.concurrent.ConcurrentHashMap;

public final class CaptionProviderService extends Service {
    static final String TAG = "MatonCaptionApp";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private final ConcurrentHashMap<Integer, TaskCaption> tasks = new ConcurrentHashMap<>();
    private final ICaptionProvider.Stub provider = new ICaptionProvider.Stub() {
        @Override public int getVersion() { return VERSION; }

        @Override public void attach(int taskId, int displayId, IBinder hostToken,
                int widthPx, int heightPx, Bundle taskState, ICaptionHost host) {
            MAIN.post(() -> attachOnMain(taskId, displayId, hostToken, widthPx, heightPx,
                    taskState, host));
        }

        @Override public void update(int taskId, int widthPx, int heightPx, Bundle taskState) {
            MAIN.post(() -> {
                TaskCaption task = tasks.get(taskId);
                if (task == null) {
                    Log.e(TAG, "update failed taskId=" + taskId + ": task is not attached");
                    return;
                }
                try { task.update(widthPx, heightPx, taskState); }
                catch (RuntimeException e) { Log.e(TAG, "update failed taskId=" + taskId, e); }
            });
        }

        @Override public void detach(int taskId) {
            MAIN.post(() -> {
                TaskCaption task = tasks.remove(taskId);
                if (task == null) return;
                try { task.release(); }
                catch (RuntimeException e) { Log.e(TAG, "detach failed taskId=" + taskId, e); }
            });
        }
    };

    @Override public IBinder onBind(Intent intent) {
        return provider;
    }

    private void attachOnMain(int taskId, int displayId, IBinder hostToken, int widthPx,
            int heightPx, Bundle state, ICaptionHost host) {
        TaskCaption old = tasks.remove(taskId);
        try {
            if (old != null) old.release();
            Display display = getSystemService(android.hardware.display.DisplayManager.class)
                    .getDisplay(displayId);
            if (display == null) throw new IllegalStateException("displayId=" + displayId + " missing");
            TaskCaption task = new TaskCaption(this, taskId, display, hostToken, host);
            tasks.put(taskId, task);
            task.update(widthPx, heightPx, state);
            host.asBinder().linkToDeath(() -> MAIN.post(() -> {
                TaskCaption dead = tasks.remove(taskId);
                Log.e(TAG, "host binder died taskId=" + taskId);
                if (dead != null) dead.release();
            }), 0);
        } catch (Exception e) {
            Log.e(TAG, "attach failed taskId=" + taskId + " displayId=" + displayId, e);
            TaskCaption task = tasks.remove(taskId);
            if (task != null) task.release();
        }
    }

    static void refreshTasks() {
        // Provider instances retain current state and redraw themselves when preferences change.
        CaptionTheme.notifyChanged();
    }
}
