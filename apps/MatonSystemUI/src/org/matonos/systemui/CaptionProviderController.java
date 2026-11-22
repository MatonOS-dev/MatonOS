/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import android.app.ActivityManager.RunningTaskInfo;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.graphics.Region;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.UserHandle;
import android.provider.Settings;
import android.util.Log;
import android.view.SurfaceControlViewHost;
import android.view.View;
import android.view.SurfaceView;
import android.view.ViewGroup;

import com.android.wm.shell.windowdecor.DesktopCaptionFactory;
import com.android.wm.shell.windowdecor.WindowDecorationActions;

import org.matonos.caption.ICaptionHost;
import org.matonos.caption.ICaptionProvider;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Binds per-task caption providers and keeps the local Shell header available as fallback. */
final class CaptionProviderController implements DesktopCaptionFactory {
    static final String TAG = "MatonCaption";
    static final String ACTION_PROVIDER = "org.matonos.caption.PROVIDER";
    static final String SETTING_PROVIDER = "matonos_caption_provider";
    static final String DEFAULT_PACKAGE = "org.matonos.caption";
    private static final long ATTACH_TIMEOUT_MS = 2000;

    private final Context mContext;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService mIpcExecutor = Executors.newCachedThreadPool();
    private final Map<Integer, Binding> mBindings = new ConcurrentHashMap<>();

    CaptionProviderController(Context context) {
        mContext = context;
    }

    @Override
    public boolean isClientDecorated(RunningTaskInfo taskInfo) {
        Binding binding = mBindings.get(taskInfo.taskId);
        return binding != null && binding.surfacePackage != null
                && taskInfo.taskDescription != null
                && (taskInfo.taskDescription.topOpaqueSystemBarsAppearance
                & android.view.WindowInsetsController.APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND)
                != 0;
    }

    @Override
    public boolean hasProviderCaption(RunningTaskInfo taskInfo) {
        Binding binding = mBindings.get(taskInfo.taskId);
        return binding != null && binding.surfacePackage != null;
    }

    @Override
    public void bindCaption(Context context, ViewGroup slot, RunningTaskInfo taskInfo,
            WindowDecorationActions actions, int widthPx, int heightPx) {
        Binding binding = mBindings.get(taskInfo.taskId);
        ComponentName desired = selectedComponent(context);
        if (binding != null && !java.util.Objects.equals(binding.requestedComponent, desired)) {
            unbindCaption(taskInfo.taskId);
            binding = null;
        }
        if (binding == null || binding.slot != slot) {
            if (binding != null) unbindCaption(taskInfo.taskId);
            binding = new Binding(context, slot, taskInfo, actions);
            mBindings.put(taskInfo.taskId, binding);
            slot.removeAllViews();
            binding.surface = new SurfaceView(context);
            binding.surface.setLayoutParams(new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            slot.addView(binding.surface);
            slot.setVisibility(View.VISIBLE);
            connect(binding, widthPx, heightPx);
        } else {
            binding.taskInfo = taskInfo;
            binding.width = widthPx;
            binding.height = heightPx;
            if (binding.provider != null) update(binding);
        }
    }

    @Override
    public void unbindCaption(int taskId) {
        Binding binding = mBindings.remove(taskId);
        if (binding == null) return;
        mHandler.removeCallbacks(binding.timeout);
        if (binding.provider != null) {
            ICaptionProvider provider = binding.provider;
            mIpcExecutor.execute(() -> {
                try {
                    provider.detach(taskId);
                } catch (Exception e) {
                    Log.e(TAG, "detach failed task=" + taskId
                            + " component=" + binding.component, e);
                }
            });
        }
        if (binding.connection != null) {
            try {
                binding.context.unbindService(binding.connection);
            } catch (IllegalArgumentException e) {
                Log.e(TAG, "unbind failed task=" + taskId + " component=" + binding.component, e);
            }
        }
        SurfaceControlViewHost.SurfacePackage p = binding.surfacePackage;
        if (p != null) binding.surface.clearChildSurfacePackage();
        binding.slot.setVisibility(View.GONE);
        setSystemHeaderState(binding, false);
    }

    private void connect(Binding b, int width, int height) {
        connect(b, width, height, selectedComponent(b.context));
    }

    private void connect(Binding b, int width, int height, ComponentName requestedComponent) {
        b.width = width;
        b.height = height;
        b.requestedComponent = requestedComponent;
        b.component = b.requestedComponent;
        Intent intent = new Intent(ACTION_PROVIDER);
        if (b.requestedComponent != null) intent.setComponent(b.requestedComponent);
        else intent.setPackage(DEFAULT_PACKAGE);
        b.connection = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                b.component = name;
                b.provider = ICaptionProvider.Stub.asInterface(service);
                try {
                    b.providerUid = b.context.getPackageManager()
                            .getApplicationInfo(name.getPackageName(), 0).uid;
                } catch (PackageManager.NameNotFoundException e) {
                    fail(b, "provider package missing", e);
                    return;
                }
                try {
                    b.host = new Host(b);
                    b.timeout = () -> {
                        if (b.surfacePackage == null) fail(b, "attach timed out", null);
                    };
                    mHandler.postDelayed(b.timeout, ATTACH_TIMEOUT_MS);
                    ICaptionProvider provider = b.provider;
                    ICaptionHost host = b.host;
                    mIpcExecutor.execute(() -> {
                        try {
                            if (provider.getVersion() != ICaptionProvider.VERSION) {
                                mHandler.post(() -> fail(b, "version mismatch", null));
                                return;
                            }
                            IBinder hostToken = b.surface.getHostToken();
                            if (hostToken == null) {
                                mHandler.post(() -> fail(b, "caption host token unavailable", null));
                                return;
                            }
                            provider.attach(b.taskInfo.taskId, b.taskInfo.displayId,
                                    hostToken, Math.max(1, b.width), Math.max(1, b.height),
                                    taskState(b.taskInfo), host);
                        } catch (Exception e) {
                            mHandler.post(() -> fail(b, "attach failed", e));
                        }
                    });
                } catch (Exception e) {
                    fail(b, "attach failed", e);
                }
            }

            @Override public void onServiceDisconnected(ComponentName name) {
                Log.e(TAG, "provider disconnected task=" + b.taskInfo.taskId + " component=" + name);
                fallback(b);
            }

            @Override public void onBindingDied(ComponentName name) {
                Log.e(TAG, "provider binding died task=" + b.taskInfo.taskId + " component=" + name);
                fallback(b);
            }

            @Override public void onNullBinding(ComponentName name) {
                Log.e(TAG, "provider returned null binding task=" + b.taskInfo.taskId
                        + " component=" + name);
                fallback(b);
            }
        };
        try {
            if (!b.context.bindServiceAsUser(intent, b.connection, Context.BIND_AUTO_CREATE,
                    UserHandle.of(b.taskInfo.userId))) {
                if (b.requestedComponent != null) {
                    Log.e(TAG, "selected provider unbindable task=" + b.taskInfo.taskId
                            + " component=" + componentLabel(b));
                    connect(b, width, height, null);
                } else {
                    fail(b, "bind returned false", null);
                }
            }
        } catch (RuntimeException e) {
            if (b.requestedComponent != null) {
                Log.e(TAG, "selected provider bind failed task=" + b.taskInfo.taskId
                        + " component=" + componentLabel(b), e);
                connect(b, width, height, null);
            } else {
                fail(b, "bind failed", e);
            }
        }
    }

    private ComponentName selectedComponent(Context context) {
        String setting = Settings.Secure.getString(context.getContentResolver(), SETTING_PROVIDER);
        return setting == null || setting.isEmpty() ? null : ComponentName.unflattenFromString(setting);
    }

    private void update(Binding b) {
        ICaptionProvider provider = b.provider;
        mIpcExecutor.execute(() -> {
            try {
                provider.update(b.taskInfo.taskId, b.width, b.height, taskState(b.taskInfo));
            } catch (Exception e) {
                mHandler.post(() -> fail(b, "update failed", e));
            }
        });
    }

    private Bundle taskState(RunningTaskInfo task) {
        Bundle state = new Bundle();
        String pkg = task.baseActivity == null ? "" : task.baseActivity.getPackageName();
        CharSequence title = task.taskDescription == null ? null : task.taskDescription.getLabel();
        Bitmap icon = task.taskDescription == null ? null : task.taskDescription.getIcon();
        state.putCharSequence("title", title);
        state.putString("package", pkg);
        state.putParcelable("icon", icon);
        state.putBoolean("focused", task.isFocused);
        state.putBoolean("maximized", task.getWindowingMode()
                == android.app.WindowConfiguration.WINDOWING_MODE_FULLSCREEN);
        state.putBoolean("clientDecorated", hasTransparentCaption(task));
        state.putBoolean("darkMode", (task.configuration.uiMode
                & android.content.res.Configuration.UI_MODE_NIGHT_MASK)
                == android.content.res.Configuration.UI_MODE_NIGHT_YES);
        state.putBoolean("buttonsAllowed", true);
        return state;
    }

    private void fail(Binding b, String reason, Throwable error) {
        Log.e(TAG, reason + " task=" + b.taskInfo.taskId + " component="
                + componentLabel(b), error);
        fallback(b);
    }

    private boolean hasTransparentCaption(RunningTaskInfo taskInfo) {
        return taskInfo.taskDescription != null
                && (taskInfo.taskDescription.topOpaqueSystemBarsAppearance
                & android.view.WindowInsetsController.APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND)
                != 0;
    }

    private String componentLabel(Binding b) {
        if (b.component != null) return b.component.flattenToShortString();
        if (b.requestedComponent != null) return b.requestedComponent.flattenToShortString();
        return DEFAULT_PACKAGE + " (action " + ACTION_PROVIDER + ")";
    }

    private void fallback(Binding b) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mHandler.post(() -> fallback(b));
            return;
        }
        mHandler.removeCallbacks(b.timeout);
        mBindings.remove(b.taskInfo.taskId, b);
        if (b.provider != null) {
            ICaptionProvider provider = b.provider;
            mIpcExecutor.execute(() -> {
                try {
                    provider.detach(b.taskInfo.taskId);
                } catch (Exception e) {
                    Log.e(TAG, "detach failed task=" + b.taskInfo.taskId
                            + " component=" + b.component, e);
                }
            });
        }
        if (b.connection != null) {
            try {
                b.context.unbindService(b.connection);
            } catch (IllegalArgumentException e) {
                Log.e(TAG, "unbind failed task=" + b.taskInfo.taskId
                        + " component=" + b.component, e);
            }
            b.connection = null;
        }
        b.provider = null;
        b.host = null;
        b.slot.setVisibility(View.GONE);
        setSystemHeaderState(b, false);
        if (b.surfacePackage != null) {
            b.surface.clearChildSurfacePackage();
            b.surfacePackage = null;
        }
    }

    private final class Host extends ICaptionHost.Stub {
        private final Binding b;
        Host(Binding binding) { b = binding; }

        private boolean allowed(int taskId) {
            if (Binder.getCallingUid() != b.providerUid || taskId != b.taskInfo.taskId
                    || !isCurrent(taskId)) {
                Log.e(TAG, "rejected host callback task=" + taskId + " component=" + b.component
                        + " uid=" + Binder.getCallingUid());
                return false;
            }
            return true;
        }

        private boolean isCurrent(int taskId) {
            return mBindings.get(taskId) == b;
        }

        @Override public void setSurface(int taskId, SurfaceControlViewHost.SurfacePackage p) {
            if (!allowed(taskId)) return;
            if (p == null) {
                mHandler.post(() -> fail(b, "provider returned null surface", null));
                return;
            }
            mHandler.post(() -> {
                if (!isCurrent(taskId)) return;
                mHandler.removeCallbacks(b.timeout);
                b.surfacePackage = p;
                b.surface.setChildSurfacePackage(p);
                p.setTouchableRegion(b.interactiveRegion);
                setSystemHeaderState(b, true);
            });
        }

        @Override public void setInteractiveRegions(int taskId, Rect[] regions) {
            if (!allowed(taskId)) return;
            Region region = new Region();
            if (regions != null) {
                for (Rect rect : regions) {
                    if (rect == null) continue;
                    region.op(rect, Region.Op.UNION);
                }
            }
            mHandler.post(() -> {
                if (!isCurrent(taskId)) return;
                b.interactiveRegion = region;
                if (b.surfacePackage != null) b.surfacePackage.setTouchableRegion(region);
            });
        }

        @Override public void close(int taskId) {
            if (allowed(taskId)) mHandler.post(() -> b.actions.onClose(b.taskInfo));
        }
        @Override public void toggleMaximize(int taskId) {
            if (allowed(taskId)) mHandler.post(() -> b.actions.onImmersiveOrRestore(b.taskInfo));
        }
        @Override public void minimize(int taskId) {
            if (allowed(taskId)) mHandler.post(() -> b.actions.onMinimize(b.taskInfo));
        }
        @Override public void openAppMenu(int taskId, Rect anchor) {
            if (allowed(taskId)) mHandler.post(() -> b.actions.onOpenHandleMenu(taskId));
        }
    }

    private void setSystemHeaderState(Binding b, boolean providerActive) {
        View root = b.slot.getRootView();
        setViewVisibility(root, "open_menu_button", !providerActive);
        boolean hideControls = providerActive && hasTransparentCaption(b.taskInfo);
        setViewVisibility(root, "close_window", !hideControls);
        setViewVisibility(root, "maximize_button_view", !hideControls);
        setViewVisibility(root, "maximize_window", !hideControls);
        setViewVisibility(root, "minimize_window", !hideControls);
    }

    private void setViewVisibility(View root, String name, boolean visible) {
        int id = root.getResources().getIdentifier(name, "id", root.getContext().getPackageName());
        View view = id == 0 ? null : root.findViewById(id);
        if (view != null) view.setVisibility(visible ? View.VISIBLE : View.GONE);
    }

    private static final class Binding {
        final Context context;
        final ViewGroup slot;
        final WindowDecorationActions actions;
        SurfaceView surface;
        RunningTaskInfo taskInfo;
        int width;
        int height;
        int providerUid;
        ComponentName component;
        ComponentName requestedComponent;
        Region interactiveRegion = new Region();
        ServiceConnection connection;
        ICaptionProvider provider;
        Host host;
        SurfaceControlViewHost.SurfacePackage surfacePackage;
        Runnable timeout = () -> {};
        Binding(Context c, ViewGroup s, RunningTaskInfo t, WindowDecorationActions a) {
            context = c; slot = s; taskInfo = t; actions = a;
        }
    }
}
