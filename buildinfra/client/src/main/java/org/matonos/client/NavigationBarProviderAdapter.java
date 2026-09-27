package org.matonos.client;

import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.view.Display;
import android.view.View;
import android.view.SurfaceControlViewHost;
import android.util.Log;

import org.matonos.systembridge.INavigationBarHostCallback;
import org.matonos.systembridge.INavigationBarProvider;

/** Typed adapter for apps that provide the system-hosted MatonOS navigation surface. */
public abstract class NavigationBarProviderAdapter {
    private static final String TAG = "MatonNavProvider";
    private final Context context;
    private final Handler main = new Handler(Looper.getMainLooper());
    private SurfaceControlViewHost host;
    private SurfaceControlViewHost.SurfacePackage activePackage;
    private INavigationBarHostCallback activeHostCallback;
    private int preferredHeightPx;
    private boolean hasPreferredHeight;

    protected NavigationBarProviderAdapter(Context context) {
        if (context == null) throw new IllegalArgumentException("provider context is required");
        this.context = context;
    }

    /** Called on the main thread. Return the provider's ready-to-embed root view. */
    protected abstract View onCreateView(int widthPx, int heightPx) throws Exception;

    /** Called on the main thread when the bridge hides or replaces this provider. */
    protected void onDetached() { }

    /** Called on the main thread while the bridge's protected host is unavailable. */
    protected void onFallbackOverlayEnabled(boolean enabled) { }

    /** Called on the main thread after the bridge applies the provider's requested size. */
    protected void onHostSizeChanged(int widthPx, int heightPx) { }

    /** Main thread: the top task changed (any launcher counts as home). */
    protected void onForegroundChanged(String packageName, boolean isHome) { }

    /** Reports the provider's desired host height in physical display pixels. */
    public final void reportPreferredHeight(int heightPx) {
        main.post(() -> {
            preferredHeightPx = heightPx;
            hasPreferredHeight = true;
            if (activeHostCallback == null) return;
            try { activeHostCallback.onPreferredHeightChanged(heightPx); }
            catch (Exception failure) {
                Log.w(TAG, "Could not report preferred navigation height", failure);
            }
        });
    }

    /** Returns a typed AIDL endpoint for a provider Service's onBind implementation. */
    public final INavigationBarProvider endpoint() {
        return new INavigationBarProvider.Stub() {
            @Override public void attach(IBinder token, int displayId, int width, int height,
                    INavigationBarHostCallback callback) {
                main.post(() -> {
                    try {
                        if (activePackage != null) activePackage.release();
                        activePackage = null;
                        if (host != null) host.release();
                        host = null;
                        activeHostCallback = callback;
                        Display display = context.getSystemService(DisplayManager.class)
                                .getDisplay(displayId);
                        if (display == null) throw new IllegalStateException("host display is unavailable");
                        host = new SurfaceControlViewHost(context, display, token);
                        View root = onCreateView(width, height);
                        if (root == null) throw new IllegalStateException("provider returned no root view");
                        host.setView(root, width, height);
                        activePackage = host.getSurfacePackage();
                        if (activePackage == null)
                            throw new IllegalStateException("provider surface package is unavailable");
                        Bundle response = new Bundle();
                        response.putParcelable("surfacePackage", activePackage);
                        callback.onSurfaceReady(response);
                        if (hasPreferredHeight)
                            callback.onPreferredHeightChanged(preferredHeightPx);
                    } catch (Exception | LinkageError failure) {
                        releaseHost();
                        Log.e(TAG, "Provider surface attach failed", failure);
                        try { callback.onSurfaceReady(Bundle.EMPTY); } catch (Exception ignored) { }
                    }
                });
            }

            @Override public void detach() {
                main.post(() -> {
                    activeHostCallback = null;
                    releaseHost();
                    try { onDetached(); }
                    catch (RuntimeException | LinkageError failure) {
                        Log.w(TAG, "Provider detach failed", failure);
                    }
                });
            }

            @Override public void onForegroundChanged(String packageName, boolean isHome) {
                main.post(() -> {
                    try { NavigationBarProviderAdapter.this.onForegroundChanged(packageName, isHome); }
                    catch (RuntimeException | LinkageError failure) {
                        Log.w(TAG, "Foreground update failed", failure);
                    }
                });
            }

            @Override public void onHostSizeChanged(int widthPx, int heightPx) {
                main.post(() -> {
                    if (host == null) return;
                    try {
                        host.relayout(widthPx, heightPx);
                        NavigationBarProviderAdapter.this.onHostSizeChanged(widthPx, heightPx);
                    } catch (RuntimeException | LinkageError failure) {
                        Log.w(TAG, "Could not relayout navigation provider host", failure);
                    }
                });
            }

            @Override public void setFallbackOverlayEnabled(boolean enabled) {
                main.post(() -> {
                    try { onFallbackOverlayEnabled(enabled); }
                    catch (RuntimeException | LinkageError failure) {
                        Log.w(TAG, "Fallback overlay state update failed", failure);
                    }
                });
            }
        };
    }

    private void releaseHost() {
        activeHostCallback = null;
        if (activePackage != null) activePackage.release();
        activePackage = null;
        if (host != null) {
            try { host.release(); } catch (RuntimeException ignored) { }
            host = null;
        }
    }
}
