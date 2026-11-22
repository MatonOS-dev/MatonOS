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
import android.provider.Settings;
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
import org.matonos.caption.ICaptionSettings;
import org.matonos.caption.ICaptionSettingsHost;

public final class CaptionSettingsSlotView extends ExpoView {
    private static final String TAG = "MatonCaptionSettings";
    private static final String ACTION = "org.matonos.caption.SETTINGS";
    private static final String DEFAULT_PACKAGE = "org.matonos.caption";
    private static final String PROVIDER_SETTING = "matonos_caption_provider";
    private final SurfaceView surface;
    private TextView status;
    private ICaptionSettings service;
    private boolean bound;
    private boolean opened;
    private boolean attached;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable openTimeout = () -> {
        if (opened && status.getVisibility() != View.VISIBLE) {
            showFailure("Caption provider did not create its settings surface");
            Log.e(TAG, "caption settings open timed out");
        }
    };

    private final ICaptionSettingsHost.Stub callback = new ICaptionSettingsHost.Stub() {
        @Override public void setSurface(SurfaceControlViewHost.SurfacePackage p) {
            post(() -> {
                try {
                    surface.setChildSurfacePackage(p);
                    status.setVisibility(View.GONE);
                    opened = true;
                    handler.removeCallbacks(openTimeout);
                } catch (RuntimeException e) {
                    showFailure("Caption provider surface could not be attached");
                    Log.e(TAG, "setChildSurfacePackage failed", e);
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
            service = ICaptionSettings.Stub.asInterface(binder);
            opened = false;
            openWhenReady();
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            service = null;
            opened = false;
            showFailure("Caption provider disconnected");
        }
        @Override public void onBindingDied(ComponentName name) {
            service = null;
            opened = false;
            showFailure("Caption provider is unavailable");
            Log.e(TAG, "binding died component=" + name);
        }
        @Override public void onNullBinding(ComponentName name) {
            showFailure("Caption provider is unavailable");
            Log.e(TAG, "null service binding component=" + name);
        }
    };

    private final SurfaceHolder.Callback surfaceCallback = new SurfaceHolder.Callback() {
        @Override public void surfaceCreated(SurfaceHolder holder) { openWhenReady(); }
        @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
            openWhenReady();
            if (service != null && opened) {
                try { service.resize(w, h); }
                catch (RemoteException e) { Log.e(TAG, "caption settings resize failed", e); showFailure("Caption provider could not resize"); }
            }
        }
        @Override public void surfaceDestroyed(SurfaceHolder holder) { closeRemote(); }
    };

    public CaptionSettingsSlotView(Context context, AppContext appContext) {
        super(context, appContext);
        setBackgroundColor(Color.TRANSPARENT);
        FrameLayout box = new FrameLayout(context);
        surface = new SurfaceView(context);
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

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        attached = true;
        bindProvider();
    }

    @Override protected void onDetachedFromWindow() {
        attached = false;
        closeRemote();
        if (bound) {
            try { getContext().unbindService(connection); }
            catch (IllegalArgumentException e) { Log.e(TAG, "unbind caption provider failed", e); }
            bound = false;
        }
        service = null;
        super.onDetachedFromWindow();
    }

    private void bindProvider() {
        String setting = Settings.Secure.getString(getContext().getContentResolver(), PROVIDER_SETTING);
        ComponentName configured = setting == null || setting.isEmpty()
                ? null : ComponentName.unflattenFromString(setting);
        String packageName = configured == null ? DEFAULT_PACKAGE : configured.getPackageName();
        Intent intent = new Intent(ACTION).setPackage(packageName);
        if (configured != null && !configured.getClassName().isEmpty()) intent.setComponent(configured);
        try {
            bound = getContext().bindService(intent, connection, Context.BIND_AUTO_CREATE);
            if (!bound) showFailure("Caption provider is not installed");
        } catch (RuntimeException e) {
            showFailure("Caption provider could not be opened");
            Log.e(TAG, "bind provider failed package=" + packageName, e);
        }
    }

    private void openWhenReady() {
        if (!attached || service == null || opened || !surface.isAttachedToWindow()) return;
        IBinder token = surface.getHostToken();
        if (token == null || surface.getWidth() <= 0 || surface.getHeight() <= 0) return;
        try {
            service.open(token, getDisplay().getDisplayId(), surface.getWidth(), surface.getHeight(), callback);
            opened = true;
            status.setVisibility(View.GONE);
            handler.removeCallbacks(openTimeout);
            handler.postDelayed(openTimeout, 2500);
        } catch (RemoteException | RuntimeException e) {
            showFailure("Caption provider could not be opened");
            Log.e(TAG, "caption settings open failed", e);
        }
    }

    private void closeRemote() {
        if (service != null && opened) {
            try { service.close(); }
            catch (RemoteException e) { Log.e(TAG, "caption settings close failed", e); }
        }
        opened = false;
        handler.removeCallbacks(openTimeout);
        surface.clearChildSurfacePackage();
    }

    private void showFailure(String text) {
        post(() -> { status.setText(text); status.setVisibility(View.VISIBLE); });
    }
}
