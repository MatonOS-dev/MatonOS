package org.matonos.compositor.stub;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import org.matonos.systembridge.ILinuxd;

/** "Install me": the store starts it right after creating the stub (and
 * again to resume an interrupted install). Any app may start it. The stub
 * sends linuxd only its package name; linuxd checks the package belongs to
 * this UID and is signed with the stub key, and reads the ref from it. The
 * foreground download service keeps this UID's network access until staging
 * is done, including when the user switches back to the store. */
public class InstallActivity extends Activity {
    private static final String TAG = "MatonStubInstall";
    private static final String LINUXD = "org.matonos.systembridge.ILinuxd/default";
    /** Optional operation ID from the store, so it can follow progress. */
    public static final String EXTRA_OPERATION_ID = "org.matonos.linuxhost.OPERATION_ID";

    protected boolean update() { return false; }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String operation = getIntent().getStringExtra(EXTRA_OPERATION_ID);
        final String operationId = operation == null ? "" : operation;
        final String pkg = getPackageName();
        final Intent download = new Intent(this, InstallService.class);
        startForegroundService(download);
        new Thread(() -> {
            try {
                // ServiceManager is hidden API; this lookup is the stub's only use of it.
                IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                        .getMethod("checkService", String.class).invoke(null, LINUXD);
                ILinuxd linuxd = ILinuxd.Stub.asInterface(binder);
                if (linuxd == null) throw new IllegalStateException("linuxd unavailable");
                String result = update() ? linuxd.updateSelf(pkg, operationId) : linuxd.installSelf(pkg, operationId);
                Log.i(TAG, (update() ? "update me: " : "install me: ") + result);
            } catch (Exception error) { Log.e(TAG, "Cannot start the Flatpak install", error); }
            finally { stopService(download); }
            runOnUiThread(this::finish);
        }).start();
    }
}
