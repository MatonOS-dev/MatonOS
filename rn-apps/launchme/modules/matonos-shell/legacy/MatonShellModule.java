package org.matonos.shell;

import android.app.WallpaperManager;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.util.Base64;
import android.util.Log;

import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContext;
import com.facebook.react.bridge.WritableArray;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.modules.core.DeviceEventManagerModule;
import com.facebook.react.module.annotations.ReactModule;
import org.matonos.systembridge.ISystemBridge;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Public Android APIs plus the narrowly scoped System Bridge for shell operations. */
@ReactModule(name = MatonShellModule.NAME)
public final class MatonShellModule extends NativeMatonShellSpec {
    public static final String NAME = "MatonShell";
    private static final String TAG = "MatonShellRN";
    private static volatile MatonShellModule active;
    private final ReactApplicationContext context;
    private final ShellBridge bridge;
    private final BroadcastReceiver packageReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context ignored, Intent intent) {
            String packageName = intent.getData() == null ? "" : intent.getData().getSchemeSpecificPart();
            if (Intent.ACTION_PACKAGE_REMOVED.equals(intent.getAction())) deleteIcon(packageName);
            WritableMap event = Arguments.createMap();
            event.putString("type", intent.getAction() == null ? "changed" : intent.getAction());
            event.putString("packageName", packageName);
            emit("packagesChanged", event);
        }
    };
    private final BroadcastReceiver wallpaperReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context ignored, Intent intent) { emitShelfEvent("wallpaperChanged"); }
    };

    MatonShellModule(ReactApplicationContext reactContext) {
        super(reactContext);
        context = reactContext;
        active = this;
        bridge = new ShellBridge(reactContext, connected -> { });
        bridge.connect();
        IntentFilter packages = new IntentFilter();
        packages.addAction(Intent.ACTION_PACKAGE_ADDED);
        packages.addAction(Intent.ACTION_PACKAGE_REMOVED);
        packages.addAction(Intent.ACTION_PACKAGE_CHANGED);
        packages.addDataScheme("package");
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(packageReceiver, packages, Context.RECEIVER_NOT_EXPORTED);
        else context.registerReceiver(packageReceiver, packages);
        IntentFilter wallpaper = new IntentFilter(Intent.ACTION_WALLPAPER_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(wallpaperReceiver, wallpaper, Context.RECEIVER_NOT_EXPORTED);
        else context.registerReceiver(wallpaperReceiver, wallpaper);
    }

    @Override public String getName() { return NAME; }

    @Override public void getLauncherApps(Promise promise) {
        try {
            Intent query = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            List<ResolveInfo> apps = context.getPackageManager().queryIntentActivities(query, 0);
            apps.removeIf(info -> context.getPackageName().equals(info.activityInfo.packageName)
                    || "com.android.systemui".equals(info.activityInfo.packageName));
            apps.sort(Comparator.comparing(info -> info.loadLabel(context.getPackageManager()).toString(),
                    String.CASE_INSENSITIVE_ORDER));
            WritableArray result = Arguments.createArray();
            for (ResolveInfo app : apps) {
                WritableMap item = Arguments.createMap();
                item.putString("packageName", app.activityInfo.packageName);
                item.putString("component", new ComponentName(app.activityInfo.packageName,
                        app.activityInfo.name).flattenToString());
                item.putString("label", app.loadLabel(context.getPackageManager()).toString());
                item.putString("iconUri", cachedIconUri(app));
                result.pushMap(item);
            }
            promise.resolve(result);
        } catch (Exception error) {
            promise.reject("APP_QUERY_FAILED", error);
        }
    }

    @Override public void launchApp(String flattenedComponent, Promise promise) {
        try {
            ComponentName component = ComponentName.unflattenFromString(flattenedComponent);
            if (component == null) { promise.resolve(false); return; }
            Intent launch = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(component);
            ShelfService.dispatchLaunch(context, launch);
            promise.resolve(true);
        } catch (RuntimeException error) {
            promise.reject("APP_LAUNCH_FAILED", error);
        }
    }

    @Override public void getRecentTasks(Promise promise) {
        ISystemBridge api = bridge.get();
        if (api == null) { promise.resolve(Arguments.createArray()); return; }
        try {
            JSONArray tasks = new JSONArray(api.getRecentTasks(32));
            Map<String, ResolveInfo> apps = new HashMap<>();
            for (ResolveInfo info : launcherApps()) apps.putIfAbsent(info.activityInfo.packageName, info);
            WritableArray result = Arguments.createArray();
            for (int i = 0; i < tasks.length(); i++) {
                JSONObject task = tasks.getJSONObject(i);
                String packageName = task.optString("packageName", "");
                ResolveInfo app = apps.get(packageName);
                if (app == null || context.getPackageName().equals(packageName)
                        || "com.android.systemui".equals(packageName)) continue;
                WritableMap item = Arguments.createMap();
                item.putInt("taskId", task.optInt("taskId", -1));
                item.putString("packageName", packageName);
                item.putString("label", app.loadLabel(context.getPackageManager()).toString());
                item.putString("component", new ComponentName(packageName,
                        app.activityInfo.name).flattenToString());
                item.putInt("windowingMode", task.optInt("windowingMode", 1));
                item.putString("iconUri", cachedIconUri(app));
                item.putString("thumbnailUri", cachedThumbnailUri(api, task.optInt("taskId", -1)));
                result.pushMap(item);
            }
            promise.resolve(result);
        } catch (Exception error) {
            Log.w(TAG, "Could not read recent tasks", error);
            promise.reject("RECENTS_UNAVAILABLE", error);
        }
    }

    @Override public void moveTaskToFront(double taskId, Promise promise) {
        ISystemBridge api = bridge.get();
        try { promise.resolve(api != null && api.moveTaskToFront((int) taskId)); }
        catch (Exception error) { promise.reject("TASK_SWITCH_FAILED", error); }
    }

    @Override public void setTaskFullscreen(double taskId, Promise promise) {
        ISystemBridge api = bridge.get();
        try { promise.resolve(api != null && api.setTaskFullscreen((int) taskId)); }
        catch (Exception error) { promise.reject("TASK_FULLSCREEN_FAILED", error); }
    }

    @Override public void closeRecentTask(double taskId, Promise promise) {
        ISystemBridge api = bridge.get();
        if (api == null) { promise.resolve(false); return; }
        try {
            Method remove = api.getClass().getMethod("removeRecentTask", int.class);
            promise.resolve((Boolean) remove.invoke(api, (int) taskId));
        } catch (Exception error) { promise.resolve(false); }
    }

    @Override public void getTaskThumbnail(double taskId, Promise promise) {
        ISystemBridge api = bridge.get();
        promise.resolve(api == null ? "" : cachedThumbnailUri(api, (int) taskId));
    }

    @Override public void getWallpaperSeedColor(Promise promise) {
        int color = 0xff6750a4;
        try {
            android.app.WallpaperColors colors = WallpaperManager.getInstance(context)
                    .getWallpaperColors(WallpaperManager.FLAG_SYSTEM);
            if (colors != null && colors.getPrimaryColor() != null)
                color = colors.getPrimaryColor().toArgb();
        } catch (RuntimeException ignored) { }
        promise.resolve(color);
    }

    @Override public void getShelfState(Promise promise) {
        WritableMap result = Arguments.createMap();
        result.putBoolean("expanded", ShelfService.isShelfExpanded());
        result.putBoolean("homeVisible", ShelfService.isHomeVisible());
        result.putString("panel", ShelfService.getActivePanel());
        promise.resolve(result);
    }

    @Override public void getPinnedApps(Promise promise) {
        WritableArray result = Arguments.createArray();
        for (String packageName : ShelfService.getPinnedApps(context)) result.pushString(packageName);
        promise.resolve(result);
    }

    @Override public void togglePinnedApp(String packageName, Promise promise) {
        promise.resolve(ShelfService.togglePinnedApp(context, packageName));
    }

    @Override public void setShelfExpanded(boolean expanded) { ShelfService.setShelfExpanded(expanded); }
    @Override public void openPanel(String panel) { ShelfService.openPanel(context, panel); }
    @Override public void goHome() { ShelfService.goHome(context); }

    @Override public void reportSurfaceFailure(String surfaceName, String error) {
        if ("shelf".equals(surfaceName)) ShelfService.fallbackToJavaShelf(error);
        else {
            Intent failed = new Intent(ShellApplication.ACTION_RN_SURFACE_FAILED)
                    .setPackage(context.getPackageName()).putExtra("surface", surfaceName)
                    .putExtra("error", error);
            context.sendBroadcast(failed);
        }
    }

    @Override public void addListener(String eventName) { }
    @Override public void removeListeners(double count) { }

    private List<ResolveInfo> launcherApps() {
        Intent query = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> apps = context.getPackageManager().queryIntentActivities(query, 0);
        apps.removeIf(info -> context.getPackageName().equals(info.activityInfo.packageName)
                || "com.android.systemui".equals(info.activityInfo.packageName));
        return apps;
    }

    private String cachedIconUri(ResolveInfo app) {
        File directory = new File(context.getCacheDir(), "launcher-icons");
        File file = new File(directory, app.activityInfo.packageName.replaceAll("[^A-Za-z0-9._-]", "_") + ".png");
        if (file.isFile()) return Uri.fromFile(file).toString();
        try {
            if (!directory.exists() && !directory.mkdirs()) return "";
            Drawable icon = app.loadIcon(context.getPackageManager());
            int size = Math.max(64, (int) (96 * context.getResources().getDisplayMetrics().density));
            Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bitmap);
            icon.setBounds(0, 0, size, size);
            icon.draw(canvas);
            try (FileOutputStream output = new FileOutputStream(file)) {
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, output);
            }
            bitmap.recycle();
            return Uri.fromFile(file).toString();
        } catch (Exception error) {
            Log.w(TAG, "Could not cache launcher icon for " + app.activityInfo.packageName, error);
            return "";
        }
    }

    private String cachedThumbnailUri(ISystemBridge api, int taskId) {
        if (taskId < 0) return "";
        try {
            Method method = api.getClass().getMethod("getRecentTaskThumbnail", int.class);
            byte[] bytes = (byte[]) method.invoke(api, taskId);
            if (bytes == null || bytes.length == 0 || bytes.length > 1_048_576) return "";
            File directory = new File(context.getCacheDir(), "task-thumbnails");
            if (!directory.exists() && !directory.mkdirs()) return "";
            File file = new File(directory, taskId + ".png");
            try (FileOutputStream output = new FileOutputStream(file)) { output.write(bytes); }
            return Uri.fromFile(file).toString();
        } catch (Exception ignored) { return ""; }
    }

    private void deleteIcon(String packageName) {
        new File(new File(context.getCacheDir(), "launcher-icons"),
                packageName.replaceAll("[^A-Za-z0-9._-]", "_") + ".png").delete();
    }

    private void emitShelfEvent(String type) {
        WritableMap event = Arguments.createMap();
        event.putString("type", type);
        event.putBoolean("expanded", ShelfService.isShelfExpanded());
        event.putBoolean("homeVisible", ShelfService.isHomeVisible());
        event.putString("panel", ShelfService.getActivePanel());
        emit("shelfStateChanged", event);
    }

    static void notifyShelfState() {
        MatonShellModule module = active;
        if (module != null) module.emitShelfEvent("stateChanged");
    }

    private void emit(String name, WritableMap value) {
        ReactContext react = getReactApplicationContext();
        if (!react.hasActiveReactInstance() && !react.isBridgeless()) return;
        react.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter.class).emit(name, value);
    }

    @Override public void invalidate() {
        try { context.unregisterReceiver(packageReceiver); } catch (Exception ignored) { }
        try { context.unregisterReceiver(wallpaperReceiver); } catch (Exception ignored) { }
        bridge.close();
        if (active == this) active = null;
        super.invalidate();
    }
}
