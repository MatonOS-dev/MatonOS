package org.matonos.systembridge;

import android.content.Context;
import android.content.Intent;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.util.Log;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Requests orphan deployment collection after PackageManager removes a stub. */
final class LinuxdDeploymentReconciler {
    private static final String TAG = "MatonLinuxd";
    private final Context context;
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private boolean removalReceiverRegistered;

    LinuxdDeploymentReconciler(Context context) { this.context = context; }

    void start() {
        android.content.IntentFilter filter = new android.content.IntentFilter(Intent.ACTION_PACKAGE_REMOVED);
        filter.addDataScheme("package");
        context.registerReceiverAsUser(removed, UserHandle.ALL, filter, null, null);
        removalReceiverRegistered = true;
        worker.scheduleWithFixedDelay(this::reconcileDeployments, 5, 30, TimeUnit.SECONDS);
    }

    private final android.content.BroadcastReceiver removed = new android.content.BroadcastReceiver() {
        @Override public void onReceive(Context ignored, Intent intent) {
            if (intent.getBooleanExtra(Intent.EXTRA_REPLACING, false) ||
                    !intent.getBooleanExtra(Intent.EXTRA_REMOVED_FOR_ALL_USERS, false)) return;
            worker.execute(LinuxdDeploymentReconciler.this::reconcileDeployments);
        }
    };

    private void reconcileDeployments() {
        try {
            ILinuxd daemon = ILinuxd.Stub.asInterface(
                    ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
            if (daemon != null) daemon.call("reconcile_deployments", "{}");
        } catch (Exception error) { Log.w(TAG, "Cannot reconcile linuxd deployments", error); }
    }

    void close() {
        if (removalReceiverRegistered) context.unregisterReceiver(removed);
        worker.shutdownNow();
    }
}
