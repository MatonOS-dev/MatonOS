package org.matonos.systembridge;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.util.Log;
import java.util.concurrent.CountDownLatch;

/**
 * Runs one authorized Flatpak install in the compositor's own app UID.
 *
 * <p>The bridge already registered the transaction with linuxd. This helper
 * binds the compositor's installer endpoint, passes only the operation ID and
 * authorized ref/remote, and blocks until the compositor reports completion or
 * failure. Both directions are authenticated: the compositor re-checks the
 * bridge UID, and this side accepts results only from the compositor UID it
 * resolved through PackageManager. Service death, not an arbitrary timeout,
 * ends a stuck operation.
 */
final class CompositorInstall {
    private static final String TAG = "MatonFlatpakStubs";
    static final String PACKAGE = "org.matonos.linux";
    private static final String SERVICE = PACKAGE + ".stub.CompositorInstallService";

    private final Context context;
    private final int compositorUid;
    private final String operationId;
    private final CountDownLatch connected = new CountDownLatch(1);
    private final CountDownLatch finished = new CountDownLatch(1);
    private volatile ICompositorInstall installer;
    private volatile String manifest;
    private volatile String failure;
    private boolean bound;

    CompositorInstall(Context context, int compositorUid, String operationId) {
        this.context = context;
        this.compositorUid = compositorUid;
        this.operationId = operationId;
    }

    /** The compositor UID the bridge authorizes, resolved through PackageManager. */
    static int uid(Context context) throws Exception {
        return context.getPackageManager().getApplicationInfo(PACKAGE, 0).uid;
    }

    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            ICompositorInstall candidate = ICompositorInstall.Stub.asInterface(service);
            if (candidate == null) { fail("The compositor installer is unavailable"); return; }
            installer = candidate;
            connected.countDown();
        }
        @Override public void onServiceDisconnected(ComponentName name) { fail("The compositor installer stopped"); }
        @Override public void onBindingDied(ComponentName name) { fail("The compositor installer binding died"); }
        @Override public void onNullBinding(ComponentName name) { fail("The compositor installer declined the binding"); }
    };

    /**
     * Accepts progress and completion only from the authorized compositor UID,
     * so a generic trusted app can never finish another compositor transaction.
     */
    private final ICompositorInstallCallback.Stub callback = new ICompositorInstallCallback.Stub() {
        @Override public void onProgress(String operation, String phase, int percent, String line) {
            if (!authenticated(operation)) return;
            SystemBridgeService.publishStoreProgress(operationId, phase, percent, line);
        }
        @Override public void onComplete(String operation, String value) {
            if (!authenticated(operation)) return;
            manifest = value;
            finished.countDown();
        }
        @Override public void onFailed(String operation, String error) {
            if (!authenticated(operation)) return;
            failure = error == null ? "Install failed" : error;
            finished.countDown();
        }
    };

    /** Blocks the caller until the compositor reports success or failure. */
    String run(String ref, String remote) throws Exception {
        Intent intent = new Intent().setComponent(new ComponentName(PACKAGE, SERVICE));
        // The explicit component resolves to this compositor package; no other
        // app can receive it, and the service itself re-checks the bridge UID.
        if (!context.bindService(intent, connection, Context.BIND_AUTO_CREATE))
            throw new IllegalStateException("Cannot bind the compositor installer");
        bound = true;
        try {
            connected.await();
            if (failure != null) throw new IllegalStateException(failure);
            installer.startInstall(callback, operationId, ref, remote);
            finished.await();
            if (failure != null) throw new IllegalStateException(failure);
            if (manifest == null || manifest.isEmpty())
                throw new IllegalStateException("The compositor returned no deployment manifest");
            return manifest;
        } finally {
            // Also covers a failed verification: the worker must not keep
            // running after the bridge has given up on the transaction.
            try { if (installer != null) installer.cancelInstall(operationId); }
            catch (Exception error) { Log.w(TAG, "Cannot cancel the compositor installer", error); }
            if (bound) {
                try { context.unbindService(connection); }
                catch (IllegalArgumentException ignored) { }
                bound = false;
            }
        }
    }

    private boolean authenticated(String operation) {
        if (android.os.Binder.getCallingUid() != compositorUid) {
            Log.w(TAG, "Ignored a result from a foreign UID for " + operationId);
            return false;
        }
        return operationId.equals(operation);
    }

    /** Counts down without touching a completion that already arrived. */
    private void fail(String message) {
        if (finished.getCount() == 0) return;
        failure = message;
        connected.countDown();
        finished.countDown();
    }
}
