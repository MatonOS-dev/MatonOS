package org.matonos.shell;

import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.View;
import android.widget.FrameLayout;
import androidx.appcompat.app.AppCompatActivity;

import com.facebook.react.ReactHost;
import com.facebook.react.ReactApplication;

/** Transparent wallpaper desktop; the React surface and service shelf share one Hermes host. */
public final class HomeActivity extends AppCompatActivity {
    private FrameLayout root;
    private ShellReactSurface surface;
    private boolean useFallback;
    private boolean desktopVisible;
    private final android.content.BroadcastReceiver failureReceiver = new android.content.BroadcastReceiver() {
        @Override public void onReceive(android.content.Context context, Intent intent) {
            if (!"home".equals(intent.getStringExtra("surface"))) return;
            useFallback = true;
            showFallback();
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Color.TRANSPARENT);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.TRANSPARENT);
        setContentView(root);
        registerReceiver(failureReceiver, new android.content.IntentFilter(MatonShellExpoModule.surfaceFailureAction()), RECEIVER_NOT_EXPORTED);
        mount();
    }

    private void mount() {
        if (useFallback) { showFallback(); return; }
        try {
            surface = ShellReactSurface.mount(this, host(), "MatonHome", new Bundle(), root, () -> {
                useFallback = true; showFallback();
            });
        } catch (RuntimeException | LinkageError error) {
            android.util.Log.e("MatonOSShell", "React home failed to initialize; using native home", error);
            useFallback = true;
            showFallback();
        }
    }

    private ReactHost host() { return ((ReactApplication) getApplication()).getReactHost(); }
    private void showFallback() {
        if (surface != null) {
            try { surface.stop(); }
            catch (RuntimeException error) { android.util.Log.w("MatonOSShell", "Could not stop failed React home", error); }
            surface = null;
        }
        root.removeAllViews();
        root.setBackgroundColor(Color.TRANSPARENT);
        root.addView(new HomeFallbackView(this), new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
    }

    @Override protected void onResume() {
        super.onResume();
        if (!useFallback) {
            try { host().onHostResume(this); }
            catch (RuntimeException | LinkageError error) {
                android.util.Log.e("MatonOSShell", "React host resume failed; using native home", error);
                useFallback = true;
                showFallback();
            }
        }
        desktopVisible = true;
        notifyShelfVisibility(true);
    }
    @Override protected void onPause() {
        desktopVisible = false;
        notifyShelfVisibility(false);
        if (!useFallback) {
            try { host().onHostPause(this); }
            catch (RuntimeException | LinkageError error) {
                android.util.Log.e("MatonOSShell", "React host pause failed", error);
                useFallback = true;
                showFallback();
            }
        }
        super.onPause();
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        // A Home request while the desktop is already visible acts as the
        // launcher's Apps/Home toggle: show the drawer over Home.
        if ("org.matonos.shell.OPEN_DRAWER".equals(intent.getAction())
                || (desktopVisible && Intent.ACTION_MAIN.equals(intent.getAction()))) openDrawer();
    }

    private void openDrawer() {
        Intent drawer = new Intent(this, DrawerActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(drawer);
    }
    @Override protected void onDestroy() {
        unregisterReceiver(failureReceiver);
        if (surface != null) {
            try { surface.stop(); }
            catch (RuntimeException error) { android.util.Log.w("MatonOSShell", "Could not stop React home", error); }
            surface = null;
        }
        super.onDestroy();
    }

    private void notifyShelfVisibility(boolean visible) {
        Intent update = new Intent("org.matonos.shelf.HOME_VISIBILITY")
                .setComponent(new android.content.ComponentName(
                        "org.matonos.shelf", "org.matonos.shelf.BootReceiver"))
                .putExtra("visible", visible);
        sendBroadcast(update);
    }
}
