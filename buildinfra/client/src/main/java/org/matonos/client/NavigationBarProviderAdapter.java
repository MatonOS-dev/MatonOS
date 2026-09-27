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

    protected NavigationBarProviderAdapter(Context context) {
        if (context == null) throw new IllegalArgumentException("provider context is required");
        this.context = context;
    }

    /** Called on the main thread. Return the provider's ready-to-embed root view. */
    protected abstract View onCreateView(int widthPx, int heightPx) throws Exception;

    /** Called on the main thread when the bridge hides or replaces this provider. */
    protected void onDetached() { }

    /** Returns a typed AIDL endpoint for a provider Service's onBind implementation. */
    public final INavigationBarProvider endpoint() {
        return new INavigationBarProvider.Stub() {
            @Override public void attach(IBinder token, int displayId, int width, int height,
                    INavigationBarHostCallback callback) {
                main.post(() -> {
                    try {
                        if (activePackage != null) activePackage.release();
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
                    } catch (Exception | LinkageError failure) {
                        releaseHost();
                        Log.e(TAG, "Provider surface attach failed", failure);
                        try { callback.onSurfaceReady(Bundle.EMPTY); } catch (Exception ignored) { }
                    }
                });
            }

            @Override public void detach() {
                main.post(() -> {
                    releaseHost();
                    try { onDetached(); }
                    catch (RuntimeException | LinkageError failure) {
                        Log.w(TAG, "Provider detach failed", failure);
                    }
                });
            }
        };
    }

    private void releaseHost() {
        if (activePackage != null) activePackage.release();
        activePackage = null;
        if (host != null) {
            try { host.release(); } catch (RuntimeException ignored) { }
            host = null;
        }
    }
}
