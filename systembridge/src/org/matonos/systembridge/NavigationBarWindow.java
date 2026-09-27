package org.matonos.systembridge;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.PixelFormat;
import android.graphics.Insets;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceControlViewHost;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.InsetsFrameProvider;
import android.widget.FrameLayout;

/** Platform-owned navigation window. Provider content and input remain embedded in its surface. */
final class NavigationBarWindow {
    private static final String TAG = "MatonNavBarHost";
    static final String DEFAULT_PROVIDER = "org.matonos.shelf";
    static final String PROVIDER_ACTION = "org.matonos.systembridge.NAVIGATION_PROVIDER";
    private final Context context;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final WindowManager windowManager;
    private FrameLayout root;
    private SurfaceView surfaceView;
    private WindowManager.LayoutParams params;
    private INavigationBarProvider provider;
    private ServiceConnection connection;
    private ServiceConnection fallbackConnection;
    private INavigationBarProvider fallbackProvider;
    private IBinder providerBinder;
    private String packageName;
    private String serviceClass;
    private String certificate;
    private boolean attached;
    private boolean surfaceRequested;
    private boolean enabled;
    private int retryCount;

    NavigationBarWindow(Context context) {
        this.context = context;
        this.windowManager = context.getSystemService(WindowManager.class);
        main.post(this::ensureShelfFallbackBound);
    }

    void select(String pkg, String cert, String providerServiceClass, boolean enabled) {
        main.post(() -> {
            detachProvider();
            packageName = pkg;
            certificate = cert;
            serviceClass = providerServiceClass;
            this.enabled = enabled;
            retryCount = 0;
            ensureShelfFallbackBound();
            if (enabled) attachProvider();
        });
    }

    void attachProvider() {
        if (!enabled || attached || packageName == null || packageName.isEmpty() || certificate == null
                || serviceClass == null || serviceClass.isEmpty()) return;
        try {
            if (!certificate.equals(SystemBridgeService.certificateFor(context, packageName))) {
                Log.e(TAG, "Selected provider certificate changed; keeping system host hidden");
                return;
            }
            createHostWindow();
            Intent intent = new Intent(PROVIDER_ACTION).setComponent(new ComponentName(packageName,
                    serviceClass));
            ServiceConnection next = new ServiceConnection() {
                @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                    main.post(() -> onProviderConnected(binder));
                }
                @Override public void onServiceDisconnected(ComponentName name) {
                    main.post(NavigationBarWindow.this::hideAndAwaitProvider);
                }
                @Override public void onBindingDied(ComponentName name) {
                    main.post(() -> {
                        detachProvider();
                        main.postDelayed(NavigationBarWindow.this::attachProvider, 1500);
                    });
                }
                @Override public void onNullBinding(ComponentName name) {
                    main.post(() -> {
                        detachProvider();
                        scheduleReattach();
                    });
                }
            };
            connection = next;
            attached = context.bindService(intent, next, Context.BIND_AUTO_CREATE | Context.BIND_IMPORTANT);
            if (!attached) {
                hideHost();
                scheduleReattach();
            }
        } catch (RuntimeException failure) {
            Log.e(TAG, "Could not attach selected navigation provider", failure);
            detachProvider();
            scheduleReattach();
        }
    }

    private void createHostWindow() {
        if (root != null) return;
        int height = Math.round(56 * context.getResources().getDisplayMetrics().density);
        root = new FrameLayout(context);
        root.setVisibility(View.INVISIBLE);
        surfaceView = new SurfaceView(context);
        surfaceView.setZOrderOnTop(false);
        root.addView(surfaceView, new FrameLayout.LayoutParams(-1, -1));
        params = new WindowManager.LayoutParams(-1, height,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.BOTTOM;
        params.setFitInsetsTypes(0);
        // Only the platform-signed bridge requests this signature permission. The Shelf app
        // never owns a system overlay window or receives SYSTEM_APPLICATION_OVERLAY.
        params.setSystemApplicationOverlay(true);
        params.providedInsets = new InsetsFrameProvider[] {
                new InsetsFrameProvider("matonos-navigation-provider", 0,
                        WindowInsets.Type.navigationBars())
                        .setSource(InsetsFrameProvider.SOURCE_FRAME)
                        .setInsetsSize(Insets.of(0, 0, 0, height))
        };
        params.setTitle("MatonOS navigation provider host");
        try {
            windowManager.addView(root, params);
            root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> requestSurfaceIfReady());
            root.post(this::requestSurfaceIfReady);
        } catch (RuntimeException failure) {
            root = null;
            surfaceView = null;
            throw failure;
        }
    }

    private void onProviderConnected(IBinder binder) {
        if (binder == null || packageName == null) return;
        if (root == null) {
            try { createHostWindow(); }
            catch (RuntimeException failure) {
                Log.e(TAG, "Could not recreate navigation host after provider restart", failure);
                detachProvider();
                return;
            }
        }
        providerBinder = binder;
        provider = INavigationBarProvider.Stub.asInterface(binder);
        surfaceRequested = false;
        retryCount = 0;
        try {
            binder.linkToDeath(() -> main.post(this::hideAndAwaitProvider), 0);
        } catch (Exception failure) {
            detachProvider();
            return;
        }
        requestSurfaceIfReady();
    }

    private void hideAndAwaitProvider() {
        hideHost();
        INavigationBarProvider oldProvider = provider;
        provider = null;
        providerBinder = null;
        surfaceRequested = false;
        if (oldProvider != null) try { oldProvider.detach(); } catch (Exception ignored) { }
        // Keep the binding. Android reconnects it when the same provider service returns.
    }

    private void requestSurfaceIfReady() {
        if (surfaceRequested || provider == null || surfaceView == null || root == null
                || root.getWidth() <= 0 || root.getHeight() <= 0) return;
        IBinder hostToken = surfaceView.getHostToken();
        Display display = windowManager.getDefaultDisplay();
        if (hostToken == null || display == null) return;
        try {
            surfaceRequested = true;
            provider.attach(hostToken, display.getDisplayId(), root.getWidth(), root.getHeight(),
                    new INavigationBarHostCallback.Stub() {
                        @Override public void onSurfaceReady(Bundle response) {
                            main.post(() -> {
                if (response == null) { detachProvider(); scheduleReattach(); return; }
                                response.setClassLoader(SurfaceControlViewHost.SurfacePackage.class.getClassLoader());
                                SurfaceControlViewHost.SurfacePackage child = response.getParcelable("surfacePackage",
                                        SurfaceControlViewHost.SurfacePackage.class);
                                if (child == null || surfaceView == null || provider == null) {
                                    detachProvider(); scheduleReattach(); return;
                                }
                                try {
                                    surfaceView.setChildSurfacePackage(child);
                                    root.setVisibility(View.VISIBLE);
                                    setFallbackOverlayEnabled(false);
                                } catch (RuntimeException failure) {
                                    Log.e(TAG, "Could not embed provider surface", failure);
                                    child.release();
                                    detachProvider();
                                    scheduleReattach();
                                }
                            });
                        }
                    });
        } catch (Exception failure) {
            surfaceRequested = false;
            Log.e(TAG, "Provider did not accept the host token", failure);
            detachProvider();
            scheduleReattach();
        }
    }

    private void scheduleReattach() {
        if (!enabled) return;
        long delayMs = Math.min(30_000L, 1_000L << Math.min(retryCount++, 5));
        main.postDelayed(() -> { if (enabled && !attached) attachProvider(); }, delayMs);
    }

    void detachProvider() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            main.post(this::detachProvider);
            return;
        }
        hideHost();
        INavigationBarProvider oldProvider = provider;
        provider = null;
        surfaceRequested = false;
        providerBinder = null;
        if (oldProvider != null) try { oldProvider.detach(); } catch (Exception ignored) { }
        if (connection != null) {
            try { context.unbindService(connection); } catch (RuntimeException ignored) { }
            connection = null;
        }
        attached = false;
    }

    void close() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            main.post(this::close);
            return;
        }
        detachProvider();
        if (fallbackConnection != null) {
            try { context.unbindService(fallbackConnection); } catch (RuntimeException ignored) { }
            fallbackConnection = null;
        }
        fallbackProvider = null;
    }

    private void ensureShelfFallbackBound() {
        if (fallbackConnection != null) return;
        String pinned = SystemBridgeService.pinnedCertificateForKey(context, "matonos-shelf");
        if (pinned == null || !pinned.equals(SystemBridgeService.certificateFor(context, DEFAULT_PROVIDER))) {
            Log.e(TAG, "Shelf fallback certificate does not match the image-pinned key");
            return;
        }
        String providerClass = SystemBridgeService.providerServiceClass(context, DEFAULT_PROVIDER);
        if (providerClass == null) {
            Log.e(TAG, "Shelf fallback provider service is unavailable");
            return;
        }
        Intent intent = new Intent(PROVIDER_ACTION).setComponent(new ComponentName(DEFAULT_PROVIDER, providerClass));
        ServiceConnection fallback = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                main.post(() -> {
                    fallbackProvider = INavigationBarProvider.Stub.asInterface(binder);
                    setFallbackOverlayEnabled(root == null || root.getVisibility() != View.VISIBLE);
                });
            }
            @Override public void onServiceDisconnected(ComponentName name) {
                main.post(() -> fallbackProvider = null);
            }
            @Override public void onBindingDied(ComponentName name) {
                main.post(() -> {
                    fallbackProvider = null;
                    if (fallbackConnection != null) {
                        try { context.unbindService(fallbackConnection); } catch (RuntimeException ignored) { }
                        fallbackConnection = null;
                    }
                    main.postDelayed(NavigationBarWindow.this::ensureShelfFallbackBound, 1500);
                });
            }
            @Override public void onNullBinding(ComponentName name) {
                main.post(() -> Log.e(TAG, "Shelf fallback returned a null binding"));
            }
        };
        try {
            if (context.bindService(intent, fallback, Context.BIND_AUTO_CREATE | Context.BIND_IMPORTANT))
                fallbackConnection = fallback;
        } catch (RuntimeException failure) {
            Log.e(TAG, "Could not bind the built-in Shelf fallback service", failure);
        }
    }

    private void setFallbackOverlayEnabled(boolean enabled) {
        INavigationBarProvider target = fallbackProvider;
        if (target == null && DEFAULT_PROVIDER.equals(packageName)) target = provider;
        if (target == null) return;
        try { target.setFallbackOverlayEnabled(enabled); }
        catch (Exception failure) { Log.w(TAG, "Could not update Shelf fallback visibility", failure); }
    }

    private void hideHost() {
        setFallbackOverlayEnabled(true);
        if (root != null) {
            try { windowManager.removeView(root); } catch (RuntimeException ignored) { }
            root = null;
            surfaceView = null;
            params = null;
        }
    }
}
