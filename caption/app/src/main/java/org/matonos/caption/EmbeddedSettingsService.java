package org.matonos.caption;

import android.app.Service;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.util.Log;
import android.view.Display;
import org.matonos.appsettings.IEmbeddedSettings;
import org.matonos.appsettings.IEmbeddedSettingsHost;

public final class EmbeddedSettingsService extends Service {
    private static final String TAG = "MatonCaptionApp";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private CaptionSettingsSession settingsSession;

    private final IEmbeddedSettings.Stub settingsBinder = new IEmbeddedSettings.Stub() {
        @Override public int getVersion() { return VERSION; }
        @Override public void open(IBinder hostToken, int displayId, int widthPx, int heightPx,
                IEmbeddedSettingsHost host) {
            MAIN.post(() -> openSettings(hostToken, displayId, widthPx, heightPx, host));
        }
        @Override public void resize(int widthPx, int heightPx) {
            MAIN.post(() -> {
                if (settingsSession == null) {
                    Log.e(TAG, "settings resize failed: no open settings surface");
                    return;
                }
                try { settingsSession.resize(widthPx, heightPx); }
                catch (RuntimeException e) { Log.e(TAG, "settings resize failed", e); }
            });
        }
        @Override public void close() {
            MAIN.post(() -> {
                if (settingsSession != null) {
                    settingsSession.release();
                    settingsSession = null;
                }
            });
        }
    };

    @Override public IBinder onBind(Intent intent) { return settingsBinder; }

    @Override public void onDestroy() {
        if (settingsSession != null) {
            settingsSession.release();
            settingsSession = null;
        }
        super.onDestroy();
    }

    private void openSettings(IBinder token, int displayId, int widthPx, int heightPx,
            IEmbeddedSettingsHost host) {
        try {
            if (settingsSession != null) settingsSession.release();
            Display display = getSystemService(android.hardware.display.DisplayManager.class)
                    .getDisplay(displayId);
            if (display == null) throw new IllegalStateException("displayId=" + displayId + " missing");
            settingsSession = new CaptionSettingsSession(this, display, token, host);
            settingsSession.resize(widthPx, heightPx);
        } catch (Exception e) {
            Log.e(TAG, "settings open failed displayId=" + displayId, e);
            settingsSession = null;
        }
    }
}
