package org.matonos.recents;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import com.facebook.react.ReactApplication;
import com.facebook.react.ReactHost;

/** AppCompat activity anchor used by SystemUI and the Shelf Recents action. */
public final class RecentsActivity extends AppCompatActivity {
    private ShellReactSurface surface;
    private FrameLayout root;
    private final android.content.BroadcastReceiver failureReceiver = new android.content.BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) { showFallback(); }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        registerReceiver(failureReceiver, new android.content.IntentFilter(MatonRecentsExpoModule.ACTION_SURFACE_FAILED), RECEIVER_NOT_EXPORTED);
        mount();
    }

    private void mount() {
        root = new FrameLayout(this);
        setContentView(root);
        try {
            Bundle props = new Bundle();
            props.putString("surface", "recents");
            surface = ShellReactSurface.mount(this, host(), "MatonRecents", props, root, this::showFallback);
        } catch (RuntimeException | LinkageError error) {
            android.util.Log.e("MatonOSRecents", "React Recents failed to initialize", error);
            showFallback();
        }
    }

    private ReactHost host() { return ((ReactApplication) getApplication()).getReactHost(); }

    private void showFallback() {
        if (surface != null) {
            try { surface.stop(); }
            catch (RuntimeException error) { android.util.Log.w("MatonOSRecents", "Could not stop failed surface", error); }
            surface = null;
        }
        TextView message = new TextView(this);
        message.setText("Recents could not load. Press Back to return to the previous app.");
        message.setTextSize(20);
        message.setGravity(Gravity.CENTER);
        root.removeAllViews();
        root.addView(message, new FrameLayout.LayoutParams(-1, -1));
    }

    @Override protected void onResume() {
        super.onResume();
        try { host().onHostResume(this); }
        catch (RuntimeException | LinkageError error) {
            android.util.Log.e("MatonOSRecents", "React Recents host resume failed", error);
            showFallback();
        }
    }

    @Override protected void onPause() {
        try { host().onHostPause(this); }
        catch (RuntimeException | LinkageError error) {
            android.util.Log.e("MatonOSRecents", "React Recents host pause failed", error);
        }
        super.onPause();
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        // Pressing App Switch while the Recents surface is already foreground
        // returns to the previously focused task, like stock three-button nav.
        finish();
    }

    @Override protected void onDestroy() {
        unregisterReceiver(failureReceiver);
        if (surface != null) { surface.stop(); surface = null; }
        super.onDestroy();
    }
}
