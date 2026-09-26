package org.matonos.shell;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import com.facebook.react.ReactApplication;
import com.facebook.react.ReactHost;

/** Hosts only the Shell app drawer; task Recents belongs to org.matonos.recents. */
public final class DrawerActivity extends AppCompatActivity {
    private ShellReactSurface surface;
    private final android.content.BroadcastReceiver failureReceiver = new android.content.BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if ("drawer".equals(intent.getStringExtra("surface"))) showFallback();
        }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        registerReceiver(failureReceiver,
                new android.content.IntentFilter(MatonShellExpoModule.surfaceFailureAction()),
                RECEIVER_NOT_EXPORTED);
        mount();
    }

    private void mount() {
        FrameLayout root = new FrameLayout(this);
        setContentView(root);
        try {
            Bundle props = new Bundle();
            props.putString("surface", "drawer");
            surface = ShellReactSurface.mount(this, host(), "MatonDrawer", props, root,
                    this::showFallback);
        } catch (RuntimeException | LinkageError error) {
            android.util.Log.e("MatonOSShell", "React drawer failed", error);
            showFallback();
        }
    }

    private ReactHost host() { return ((ReactApplication) getApplication()).getReactHost(); }

    private void showFallback() {
        if (surface != null) {
            try { surface.stop(); }
            catch (RuntimeException error) { android.util.Log.w("MatonOSShell", "Could not stop failed drawer surface", error); }
            surface = null;
        }
        TextView fallback = new TextView(this);
        fallback.setText("App drawer failed to load. Press Home and try again.");
        fallback.setTextSize(20);
        fallback.setPadding(32, 32, 32, 32);
        setContentView(fallback);
    }

    @Override protected void onResume() {
        super.onResume();
        try { host().onHostResume(this); }
        catch (RuntimeException | LinkageError error) {
            android.util.Log.e("MatonOSShell", "React drawer host resume failed", error);
            showFallback();
        }
    }

    @Override protected void onPause() {
        try { host().onHostPause(this); }
        catch (RuntimeException | LinkageError error) {
            android.util.Log.e("MatonOSShell", "React drawer host pause failed", error);
        }
        super.onPause();
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        // A second Apps request collapses the drawer back to Home.
        finish();
    }

    @Override protected void onDestroy() {
        unregisterReceiver(failureReceiver);
        if (surface != null) { surface.stop(); surface = null; }
        super.onDestroy();
    }
}
