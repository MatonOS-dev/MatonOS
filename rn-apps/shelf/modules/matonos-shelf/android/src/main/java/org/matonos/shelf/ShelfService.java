package org.matonos.shelf;

import android.app.ActivityOptions;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;

import com.facebook.react.ReactHost;
import com.facebook.react.ReactApplication;
import org.matonos.systembridge.ISystemBridge;
import org.matonos.client.MatonosClient;
import org.matonos.client.NavigationBarProviderAdapter;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

/** Provider UI hosted by the system bridge; a regular overlay remains as an unavailable-bridge fallback. */
public final class ShelfService extends Service {
    static final String ACTION_HOME_VISIBLE = "org.matonos.shelf.HOME_VISIBILITY";
    static final String ACTION_TOGGLE_PIN = "org.matonos.shelf.TOGGLE_PIN";
    static final String EXTRA_HOME_VISIBLE = "visible";
    private static volatile ShelfService active;
    private WindowManager wm;
    private View content;
    private android.widget.FrameLayout shelfRoot;
    private ShelfView javaFallback;
    private ShellReactSurface rnSurface;
    private WindowManager.LayoutParams params;
    private ShellBridge bridge;
    private boolean bridgeReady;
    private android.widget.FrameLayout providerRoot;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private boolean added, homeVisible, expanded = true, rnFailed;
    private String panel = "";
    private static final String PREFS = "shelf";
    private static final String PINS = "pinned";
    private static final String THREE_BUTTON = "three_button_mode";
    private static final String THREE_BUTTON_OVERRIDE = "three_button_mode_override";
    private final NavigationBarProviderAdapter providerAdapter = new NavigationBarProviderAdapter(this) {
        @Override protected View onCreateView(int widthPx, int heightPx) {
            removeFallbackOverlay();
            providerRoot = new android.widget.FrameLayout(ShelfService.this);
            View providerContent = createShelfContent(providerRoot);
            if (providerContent == javaFallback) providerRoot.addView(javaFallback,
                    new android.widget.FrameLayout.LayoutParams(-1, -1));
            return providerRoot;
        }

        @Override protected void onDetached() { releaseProviderSurface(); }
        @Override protected void onFallbackOverlayEnabled(boolean enabled) {
            if (enabled) installFallbackOverlay(); else removeFallbackOverlay();
        }
    };
    @Override public void onCreate() {
        super.onCreate(); active = this;
        wm = getSystemService(WindowManager.class);
        javaFallback = new ShelfView(this);
        try { host().onHostResume((android.app.Activity) null); }
        catch (RuntimeException | LinkageError failure) {
            rnFailed = true;
            android.util.Log.e("MatonOSShelf", "React host could not enter service lifecycle; using Java shelf", failure);
        }
        params = new WindowManager.LayoutParams(WindowManager.LayoutParams.MATCH_PARENT, dp(56),
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.BOTTOM; params.setFitInsetsTypes(0); params.setTitle("MatonOS shelf fallback");
        bridge = new ShellBridge(this, this::onBridgeAvailabilityChanged); bridge.connect();
        // The bridge should control this fallback through the typed provider adapter; retain
        // a local watchdog too, so attach failures never leave the user without navigation.
        mainHandler.postDelayed(() -> {
            if (!providerSurfaceAttached() && !added) installFallbackOverlay();
        }, 7000);
    }

    private void onBridgeAvailabilityChanged(boolean available) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(() -> onBridgeAvailabilityChanged(available));
            return;
        }
        bridgeReady = available;
        if (!available && !added) installFallbackOverlay();
    }

    private void installFallbackOverlay() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(this::installFallbackOverlay);
            return;
        }
        if (added) return;
        if (!Settings.canDrawOverlays(this)) {
            if (bridge != null) {
                org.matonos.client.MatonosClient.Result<Boolean> result = bridge.ensureShellOverlayAccess();
                if (!result.available || !Boolean.TRUE.equals(result.value))
                    android.util.Log.w("MatonOSShelf", "Bridge could not prepare overlay access: " + result.reason);
            }
            if (!Settings.canDrawOverlays(this)) return;
        }
        // Keep the fallback deliberately simple and independent from the provider's RN
        // SurfaceControlViewHost lifecycle. The Java controls remain usable during detach,
        // React reloads, attach errors, or bridge restarts.
        content = javaFallback;
        if (content.getParent() != null) {
            content = new ShelfView(this);
            ((ShelfView) content).setHomeVisible(homeVisible);
            ((ShelfView) content).setExpanded(expanded);
        }
        try { wm.addView(content, params); added = true; }
        catch (WindowManager.BadTokenException | SecurityException error) {
            android.util.Log.e("MatonOSShell", "Cannot attach shelf overlay", error);
        }
    }

    private View createShelfContent(android.widget.FrameLayout target) {
        if (!rnFailed) {
            try {
                Bundle props = new Bundle(); props.putString("surface", "shelf");
                props.putString("component", "MatonShelf");
                android.widget.FrameLayout parent = target == null
                        ? new android.widget.FrameLayout(this) : target;
                rnSurface = ShellReactSurface.mount(this, host(), "MatonShelf", props,
                        parent, () -> switchToJavaShelf("surface start failed"));
                if (target == null) shelfRoot = parent;
                return parent;
            } catch (RuntimeException | LinkageError failure) {
                rnFailed = true;
                android.util.Log.e("MatonOSShell", "RN shelf failed; using Java fallback", failure);
            }
        }
        return javaFallback;
    }

    private void removeFallbackOverlay() {
        if (!added || content == null) return;
        try { wm.removeView(content); } catch (RuntimeException ignored) { }
        if (rnSurface != null) { rnSurface.stop(); rnSurface = null; }
        if (shelfRoot != null) shelfRoot.removeAllViews();
        shelfRoot = null; content = null; added = false;
    }

    private void releaseProviderSurface() {
        if (rnSurface != null) { rnSurface.stop(); rnSurface = null; }
        if (providerRoot != null) providerRoot.removeAllViews();
        providerRoot = null;
    }

    void refreshOverlayAccess() {
        if (!added) installFallbackOverlay();
    }

    private ReactHost host() { return ((ReactApplication) getApplication()).getReactHost(); }
    private void switchToJavaShelf(String reason) {
        android.util.Log.e("MatonOSShell", "RN shelf failed; switching to Java fallback: " + reason);
        rnFailed = true;
        if (rnSurface != null) {
            try { rnSurface.stop(); }
            catch (RuntimeException error) { android.util.Log.w("MatonOSShell", "Could not stop failed React shelf", error); }
            rnSurface = null;
        }
        if (providerRoot != null) {
            providerRoot.removeAllViews();
            if (javaFallback.getParent() instanceof android.view.ViewGroup)
                ((android.view.ViewGroup) javaFallback.getParent()).removeView(javaFallback);
            providerRoot.addView(javaFallback, new android.widget.FrameLayout.LayoutParams(-1, -1));
            return;
        }
        if (added && content != null) try { wm.removeView(content); } catch (RuntimeException ignored) { }
        if (shelfRoot != null) shelfRoot.removeAllViews();
        content = javaFallback; added = false;
        if (content != null && Settings.canDrawOverlays(this)) try { wm.addView(content, params); added = true; }
        catch (RuntimeException error) { android.util.Log.e("MatonOSShell", "Java shelf fallback attach failed", error); }
    }

    static void fallbackToJavaShelf(String reason) { ShelfService service = active; if (service != null) service.switchToJavaShelf(reason); }
    private boolean providerSurfaceAttached() { return providerRoot != null && providerRoot.isAttachedToWindow(); }
    static void dispatchLaunch(Context context, Intent intent) { ShelfService service = active; if (service != null) service.startShellActivity(intent); else context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); }
    static void openPanel(Context context, String target) {
        ShelfService service = active;
        if (service != null && target.equals(service.panel)) {
            service.panel = "";
            goHome(context);
            MatonShelfExpoModule.notifyShelfState();
            return;
        }
        if (service != null) { service.panel = target; MatonShelfExpoModule.notifyShelfState(); }
        android.content.ComponentName component = "recents".equals(target)
                ? new android.content.ComponentName("org.matonos.recents", "org.matonos.recents.RecentsActivity")
                : new android.content.ComponentName("org.matonos.shell", "org.matonos.shell.DrawerActivity");
        Intent intent = new Intent().setComponent(component);
        if (service != null) service.startShellActivity(intent); else context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }
    static void goHome(Context context) {
        ShelfService service = active;
        Intent home = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME).addCategory(Intent.CATEGORY_DEFAULT);
        if (service != null) service.startShellActivity(home); else context.startActivity(home.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }
    static boolean isShelfExpanded() { return active != null && active.expanded; }
    static boolean isHomeVisible() { return active != null && active.homeVisible; }
    static String getActivePanel() { return active == null ? "" : active.panel; }
    static boolean isThreeButtonMode(Context context) {
        android.content.SharedPreferences preferences = context.getSharedPreferences(PREFS, MODE_PRIVATE);
        if (preferences.getBoolean(THREE_BUTTON_OVERRIDE, false))
            return preferences.getBoolean(THREE_BUTTON, false);
        // Match the classic shelf controls to AOSP's selected navigation mode
        // until the user explicitly chooses a Shelf layout in Shelf Settings.
        return Settings.Secure.getInt(context.getContentResolver(), "navigation_mode", 2) == 0;
    }
    static void setThreeButtonMode(Context context, boolean enabled) {
        context.getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putBoolean(THREE_BUTTON, enabled).putBoolean(THREE_BUTTON_OVERRIDE, true).apply();
        if (active != null) active.refreshJavaFallback();
        MatonShelfExpoModule.notifyShelfState();
    }
    private void refreshJavaFallback() {
        if (providerRoot != null && providerRoot.getChildCount() == 1
                && providerRoot.getChildAt(0) == javaFallback) {
            providerRoot.removeView(javaFallback);
            boolean oldExpanded = expanded;
            javaFallback = new ShelfView(this);
            javaFallback.setHomeVisible(homeVisible);
            javaFallback.setExpanded(oldExpanded);
            providerRoot.addView(javaFallback, new android.widget.FrameLayout.LayoutParams(-1, -1));
            return;
        }
        boolean fallbackVisible = content == javaFallback;
        if (fallbackVisible && added) try { wm.removeView(content); } catch (RuntimeException ignored) { }
        boolean oldExpanded = expanded;
        javaFallback = new ShelfView(this);
        javaFallback.setHomeVisible(homeVisible);
        javaFallback.setExpanded(oldExpanded);
        if (fallbackVisible && Settings.canDrawOverlays(this)) try {
            content = javaFallback;
            wm.addView(content, params);
            added = true;
        } catch (RuntimeException error) { android.util.Log.e("MatonOSShelf", "Could not refresh Java shelf", error); }
    }

    boolean navigate(String action) {
        return bridge != null && bridge.navigate(action);
    }
    boolean navigate(String action, boolean longPress) {
        return bridge != null && bridge.navigate(action, longPress);
    }
    static Set<String> getPinnedApps(Context context) { return new LinkedHashSet<>(context.getSharedPreferences(PREFS, MODE_PRIVATE).getStringSet(PINS, Collections.emptySet())); }
    static boolean togglePinnedApp(Context context, String pkg) {
        Set<String> pins = getPinnedApps(context); boolean nowPinned = pins.add(pkg); if (!nowPinned) pins.remove(pkg);
        context.getSharedPreferences(PREFS, MODE_PRIVATE).edit().putStringSet(PINS, pins).apply();
        return nowPinned;
    }
    static boolean handleBroadcast(Intent intent) {
        ShelfService service = active;
        if (service == null || intent == null) return false;
        if (ACTION_HOME_VISIBLE.equals(intent.getAction())) {
            service.updateHomeVisibility(intent.getBooleanExtra(EXTRA_HOME_VISIBLE, false));
            return true;
        }
        if (ACTION_TOGGLE_PIN.equals(intent.getAction())) {
            String packageName = intent.getStringExtra("packageName");
            if (packageName != null) togglePinnedApp(service, packageName);
            MatonShelfExpoModule.notifyShelfState();
            return true;
        }
        return false;
    }
    static void setShelfExpanded(boolean value) { if (active != null) { active.expanded = value; active.updateShelfHeight(active.dp(value || active.homeVisible ? 56 : 28)); MatonShelfExpoModule.notifyShelfState(); } }
    static ISystemBridge getBridge() { return active == null || active.bridge == null ? null : active.bridge.get(); }

    private void updateHomeVisibility(boolean visible) {
        homeVisible = visible;
        if (visible) panel = "";
        expanded = visible;
        updateShelfHeight(dp(visible ? 56 : 28));
        if (javaFallback != null) javaFallback.setHomeVisible(visible);
        MatonShelfExpoModule.notifyShelfState();
    }

    void expandShelfTemporarily() { expanded = true; updateShelfHeight(dp(56)); if (javaFallback != null) javaFallback.setExpanded(true); }
    void collapseShelfTemporarily() { if (!homeVisible) { expanded = false; updateShelfHeight(dp(28)); if (javaFallback != null) javaFallback.setExpanded(false); } }
    void startShellActivity(Intent intent) {
        try {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            PendingIntent pending = PendingIntent.getActivity(this, intent.filterHashCode(), intent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            ActivityOptions options = ActivityOptions.makeBasic().setPendingIntentBackgroundActivityStartMode(4);
            pending.send(this, 0, null, null, null, null, options.toBundle());
        } catch (PendingIntent.CanceledException | RuntimeException error) { android.util.Log.w("MatonOSShell", "Shelf activity launch rejected", error); }
    }
    private void updateShelfHeight(int height) {
        if (!added || params == null || content == null || params.height == height) return;
        params.height = height; try { wm.updateViewLayout(content, params); } catch (RuntimeException ignored) { }
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_HOME_VISIBLE.equals(intent.getAction()))
            updateHomeVisibility(intent.getBooleanExtra(EXTRA_HOME_VISIBLE, false));
        if (intent != null && ACTION_TOGGLE_PIN.equals(intent.getAction())) {
            String packageName = intent.getStringExtra("packageName");
            if (packageName != null) togglePinnedApp(this, packageName);
            MatonShelfExpoModule.notifyShelfState();
        }
        return START_NOT_STICKY;
    }
    int dp(int value) { return (int) (value * getResources().getDisplayMetrics().density + .5f); }
    ISystemBridge systemBridge() { return bridge == null ? null : bridge.get(); }
    @Override public void onDestroy() {
        if (added && content != null) wm.removeView(content);
        releaseProviderSurface();
        if (rnSurface != null) rnSurface.stop(); if (bridge != null) bridge.close(); active = null;
        try { host().onHostPause((android.app.Activity) null); }
        catch (RuntimeException | LinkageError error) { android.util.Log.w("MatonOSShelf", "React host pause failed", error); }
        super.onDestroy();
    }
    @Override public IBinder onBind(Intent intent) {
        return MatonosClient.createNavigationBarProvider(providerAdapter).asBinder();
    }
}
