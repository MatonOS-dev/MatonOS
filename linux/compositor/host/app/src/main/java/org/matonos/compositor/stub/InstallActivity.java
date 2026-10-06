package org.matonos.compositor.stub;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import org.matonos.compositor.IEmbeddedHost;

/** "Install me": the store starts it right after creating the stub (and
 * again to resume an interrupted install). Any app may start it. The bridge
 * derives everything from the verified stub and does nothing when the
 * Flatpak is already installed, so it needs no permission. */
public final class InstallActivity extends Activity {
    private static final String TAG = "MatonStubInstall";
    private String ref;
    private boolean bound;
    private final ServiceConnection connection = new ServiceConnection() {
        public void onServiceConnected(ComponentName name, IBinder binder) {
            new Thread(() -> {
                try { IEmbeddedHost.Stub.asInterface(binder).installSelf(ref); }
                catch (Exception error) { Log.e(TAG, "Cannot start the Flatpak install", error); }
                runOnUiThread(InstallActivity.this::finish);
            }).start();
        }
        public void onServiceDisconnected(ComponentName name) { }
    };
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            ActivityInfo info = getPackageManager().getActivityInfo(getComponentName(), PackageManager.GET_META_DATA);
            if (info.metaData != null) ref = info.metaData.getString(HostContract.META_FLATPAK_REF);
        } catch (Exception error) { Log.e(TAG, "Cannot read application reference", error); }
        if (ref == null || ref.trim().isEmpty()) { finish(); return; }
        Intent host = new Intent("org.matonos.compositor.EMBEDDED")
                .setClassName("org.matonos.compositor", "org.matonos.compositor.CompositorService");
        try {
            startForegroundService(host);
            bound = bindService(host, connection, BIND_AUTO_CREATE);
        } catch (Exception error) { Log.e(TAG, "Cannot connect to the compositor", error); }
        if (!bound) finish();
    }
    @Override protected void onDestroy() {
        if (bound) unbindService(connection);
        super.onDestroy();
    }
}
