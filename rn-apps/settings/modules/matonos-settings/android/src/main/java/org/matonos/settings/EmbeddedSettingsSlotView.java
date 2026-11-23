package org.matonos.settings;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Color;
import android.os.IBinder;
import android.os.Handler;
import android.os.Looper;
import android.os.RemoteException;
import android.util.Log;
import android.view.SurfaceControlViewHost;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.view.SurfaceView;
import android.widget.TextView;
import expo.modules.kotlin.AppContext;
import expo.modules.kotlin.views.ExpoView;
import org.matonos.appsettings.IEmbeddedSettings;
import org.matonos.appsettings.IEmbeddedSettingsHost;

public final class EmbeddedSettingsSlotView extends ExpoView {
    private static final String TAG = "MatonEmbeddedSettings";
    private static final String ACTION = "org.matonos.appsettings.EMBEDDED_SETTINGS";
    private final SurfaceView surface;
    private final TextView status;
    private IEmbeddedSettings service;
    private ComponentName component;
    private boolean bound;
    private boolean opened;
    private boolean attached;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable openTimeout = () -> {
        if (opened && status.getVisibility() != View.VISIBLE) {
            showFailure("App did not create its settings surface");
            Log.e(TAG, "settings surface open timed out component=" + component);
        }
    };

    private final IEmbeddedSettingsHost.Stub callback = new IEmbeddedSettingsHost.Stub() {
        @Override public void setSurface(SurfaceControlViewHost.SurfacePackage p) {
            post(() -> {
                try {
                    surface.setChildSurfacePackage(p);
                    status.setVisibility(View.GONE);
                    opened = true;
                    handler.removeCallbacks(openTimeout);
                } catch (RuntimeException e) {
                    showFailure("App settings surface could not be attached");
                    Log.e(TAG, "setChildSurfacePackage failed component=" + component, e);
                }
            });
        }
        @Override public void setPreferredHeight(int heightPx) {
            post(() -> {
                ViewGroup.LayoutParams lp = getLayoutParams();
                if (lp != null && heightPx > 0 && lp.height > 0 && lp.height != heightPx) {
                    lp.height = heightPx;
                    setLayoutParams(lp);
                }
            });
        }
    };

    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            service = IEmbeddedSettings.Stub.asInterface(binder);
            opened = false;
            openWhenReady();
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            service = null;
            opened = false;
            showFailure("App settings service disconnected");
            Log.e(TAG, "service disconnected component=" + name);
        }
        @Override public void onBindingDied(ComponentName name) {
            service = null;
            opened = false;
            showFailure("App settings service is unavailable");
            Log.e(TAG, "binding died component=" + name);
        }
        @Override public void onNullBinding(ComponentName name) {
            showFailure("App settings service is unavailable");
            Log.e(TAG, "null service binding component=" + name);
        }
    };

    private final SurfaceHolder.Callback surfaceCallback = new SurfaceHolder.Callback() {
        @Override public void surfaceCreated(SurfaceHolder holder) { openWhenReady(); }
        @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
            openWhenReady();
            if (service != null && opened) {
                try { service.resize(w, h); }
                catch (RemoteException e) {
                    Log.e(TAG, "settings resize failed component=" + component, e);
                    showFailure("App settings could not resize");
                }
            }
        }
        @Override public void surfaceDestroyed(SurfaceHolder holder) { closeRemote(); }
    };

    public EmbeddedSettingsSlotView(Context context, AppContext appContext) {
        super(context, appContext);
        setBackgroundColor(Color.TRANSPARENT);
        FrameLayout box = new FrameLayout(context);
        surface = new SurfaceView(context);
        surface.setZOrderOnTop(true);
        surface.getHolder().setFormat(android.graphics.PixelFormat.TRANSLUCENT);
        box.addView(surface, new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        status = new TextView(context);
        status.setTextColor(Color.GRAY);
        status.setTextSize(14);
        status.setGravity(android.view.Gravity.CENTER_VERTICAL);
        status.setVisibility(View.GONE);
        box.addView(status, new FrameLayout.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        addView(box, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        surface.getHolder().addCallback(surfaceCallback);
    }

    @Override public boolean getShouldUseAndroidLayout() { return true; }

    public void setComponentName(String flattened) {
        ComponentName next = ComponentName.unflattenFromString(flattened);
        if (next == null) {
            Log.e(TAG, "invalid embedded settings component=" + flattened);
            showFailure("App settings provider is invalid");
            return;
        }
        if (next.equals(component)) return;
        closeRemote();
        unbindProvider();
        component = next;
        if (attached) bindProvider();
    }

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        attached = true;
        bindProvider();
    }

    @Override protected void onDetachedFromWindow() {
        attached = false;
        closeRemote();
        unbindProvider();
        service = null;
        super.onDetachedFromWindow();
    }

    private void bindProvider() {
        if (component == null || bound) return;
        Intent intent = new Intent(ACTION).setComponent(component);
        try {
            bound = getContext().bindService(intent, connection, Context.BIND_AUTO_CREATE);
            if (!bound) showFailure("App settings provider is unavailable");
        } catch (RuntimeException e) {
            showFailure("App settings could not be opened");
            Log.e(TAG, "bind failed component=" + component, e);
        }
    }

    private void unbindProvider() {
        if (!bound) return;
        try { getContext().unbindService(connection); }
        catch (IllegalArgumentException e) { Log.e(TAG, "unbind failed component=" + component, e); }
        bound = false;
    }

    private void openWhenReady() {
        if (!attached || component == null || service == null || opened || !surface.isAttachedToWindow()) return;
        IBinder token = surface.getHostToken();
        if (token == null || surface.getWidth() <= 0 || surface.getHeight() <= 0) return;
        try {
            service.open(token, getDisplay().getDisplayId(), surface.getWidth(), surface.getHeight(), callback);
            opened = true;
            status.setVisibility(View.GONE);
            handler.removeCallbacks(openTimeout);
            handler.postDelayed(openTimeout, 2500);
        } catch (RemoteException | RuntimeException e) {
            showFailure("App settings could not be opened");
            Log.e(TAG, "open failed component=" + component, e);
        }
    }

    private void closeRemote() {
        if (service != null && opened) {
            try { service.close(); }
            catch (RemoteException e) { Log.e(TAG, "close failed component=" + component, e); }
        }
        opened = false;
        handler.removeCallbacks(openTimeout);
        surface.clearChildSurfacePackage();
    }

    private void showFailure(String text) {
        post(() -> { status.setText(text); status.setVisibility(View.VISIBLE); });
    }
}
