package org.matonos.compositor.stub;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import org.matonos.systembridge.ICompositorInstall;
import org.matonos.systembridge.ICompositorInstallCallback;

/**
 * The System Bridge's install endpoint inside the compositor.
 *
 * <p>Only the authenticated bridge may drive this service: the caller UID must
 * be the bridge's own PackageManager UID, and the manifest additionally
 * requires {@code org.matonos.permission.SYSTEM_BRIDGE}. The service owns at
 * most one transaction, keeps a wake lock and the progress notification for its
 * lifetime, and never sees a caller-supplied path, loader flag or final
 * identity. Everything else lives in {@link InstallTransaction}.
 */
public final class CompositorInstallService extends Service {
    private static final String TAG = "MatonInstall";
    private static final String CHANNEL = "linux-install";
    private static final int NOTIFICATION_ID = 3;
    private static final String BRIDGE_PACKAGE = "org.matonos.systembridge";

    private final Object lock = new Object();
    private InstallTransaction active;

    private final ICompositorInstall.Stub binder = new ICompositorInstall.Stub() {
        @Override public void startInstall(ICompositorInstallCallback callback, String operationId,
                String appRef, String remote) {
            requireBridge();
            InstallTransaction transaction = InstallTransaction.create(
                    CompositorInstallService.this, callback, operationId, appRef, remote);
            synchronized (lock) {
                if (active != null) throw new IllegalStateException("Another install is active");
                active = transaction;
            }
            beginForeground();
            try {
                transaction.start(() -> finish(transaction));
            } catch (RuntimeException error) {
                finish(transaction);
                throw error;
            }
        }

        @Override public void cancelInstall(String operationId) {
            requireBridge();
            InstallTransaction transaction;
            synchronized (lock) { transaction = active; }
            if (transaction != null) transaction.cancel(operationId);
        }
    };

    @Override public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override public void onDestroy() {
        InstallTransaction transaction;
        synchronized (lock) { transaction = active; active = null; }
        if (transaction != null) transaction.cancel();
        super.onDestroy();
    }

    /** The bridge's UID is authoritative; the permission only narrows the set. */
    private void requireBridge() {
        int bridge;
        try {
            bridge = getPackageManager().getApplicationInfo(BRIDGE_PACKAGE, 0).uid;
        } catch (PackageManager.NameNotFoundException missing) {
            throw new SecurityException("System bridge is not installed");
        }
        if (Binder.getCallingUid() != bridge)
            throw new SecurityException("Only the System Bridge may install Linux applications");
    }

    private void beginForeground() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                "Linux application downloads", NotificationManager.IMPORTANCE_LOW));
        Notification notification = new Notification.Builder(this, CHANNEL)
                .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                .setContentText("Downloading Linux application")
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true).build();
        startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
    }

    /** Runs on the worker thread once for the transaction that just finished. */
    private void finish(InstallTransaction transaction) {
        synchronized (lock) { if (active == transaction) active = null; }
        try { stopForeground(STOP_FOREGROUND_REMOVE); }
        catch (RuntimeException error) { Log.w(TAG, "Cannot clear the install notification", error); }
        stopSelf();
    }
}
