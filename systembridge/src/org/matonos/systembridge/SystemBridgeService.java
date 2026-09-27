package org.matonos.systembridge;

import android.app.ActivityManager;
import android.app.ActivityTaskManager;
import android.app.AppOpsManager;
import android.app.WindowConfiguration;
import android.app.Service;
import android.hardware.display.DisplayManager;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.hardware.input.InputManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.SystemClock;
import android.os.UserHandle;
import android.provider.Settings;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.Display;
import android.graphics.Point;
import android.graphics.Bitmap;
import android.window.TaskSnapshot;
import android.window.TaskSnapshotManager;
import android.window.WindowContainerTransaction;
import android.window.WindowOrganizer;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.security.MessageDigest;
import java.util.List;
import java.util.HashSet;
import java.util.Map;
import java.util.Locale;
import java.nio.charset.StandardCharsets;
import java.io.ByteArrayOutputStream;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

import vendor.matonos.channel.IChannel;
import vendor.matonos.channel.IChannelListener;

public final class SystemBridgeService extends Service {
    private static final String TAG = "MatonSystemBridge";
    private static final String PERMISSION = "org.matonos.permission.SYSTEM_BRIDGE";
    private static final int MAX_EXPOSED_TASKS = 50;
    private volatile Set<String> allowedCerts;
    private volatile Set<String> targetCallers;
    private android.os.Handler geometryHandler;
    private boolean geometryRetryPending;
    private final ConcurrentMap<String, Subscription> subscriptions = new ConcurrentHashMap<>();
    private NavigationBarWindow navigationBarWindow;
    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int displayId) { publishDisplayGeometry(); }
        @Override public void onDisplayRemoved(int displayId) { publishDisplayGeometry(); }
        @Override public void onDisplayChanged(int displayId) { publishDisplayGeometry(); }
    };

    private final ISystemBridge.Stub binder = new ISystemBridge.Stub() {
        @Override public int getBridgeApiVersion() {
            enforceNotBanned(Binder.getCallingUid(), "getBridgeApiVersion");
            return 6;
        }
        @Override public String getBridgeApiHash() {
            enforceNotBanned(Binder.getCallingUid(), "getBridgeApiHash");
            return "nav-provider-v1-20260927";
        }

        @Override public boolean injectBackKey() {
            enforceAuthorizedCaller("input", "injectBackKey");
            long now = SystemClock.uptimeMillis();
            KeyEvent down = new KeyEvent(now, now, KeyEvent.ACTION_DOWN,
                    KeyEvent.KEYCODE_BACK, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
                    KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD);
            KeyEvent up = KeyEvent.changeAction(down, KeyEvent.ACTION_UP);
            InputManager input = getSystemService(InputManager.class);
            boolean first = input.injectInputEvent(down, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
            boolean second = input.injectInputEvent(up, InputManager.INJECT_INPUT_EVENT_MODE_ASYNC);
            return first && second;
        }

        @Override public String getBridgeStatus() {
            int uid = Binder.getCallingUid();
            enforcePermission(uid, "getBridgeStatus");
            enforceNotBanned(uid, "getBridgeStatus");
            JSONArray allowed = new JSONArray();
            JSONArray channels = new JSONArray();
            for (String target : knownTargets()) {
                if (authorizedPackage(uid, target) != null) allowed.put(target);
                if (channelFor(target) != null && authorizedPackage(uid, target) != null) channels.put(target);
            }
            if (allowed.length() == 0) Log.w(TAG, "Denied getBridgeStatus for uid=" + uid
                    + ": no built-in or developer trust entry matches caller");
            JSONObject status = new JSONObject();
            try { status.put("apiVersion", 5); status.put("allowedTargets", allowed);
                status.put("availableChannels", channels); }
            catch (JSONException ignored) { }
            return status.toString();
        }

        @Override public boolean ensureShellOverlayAccess() {
            String packageName = enforceLauncherCaller("ensureShellOverlayAccess");
            if (!"org.matonos.shelf".equals(packageName))
                deny("ensureShellOverlayAccess", Binder.getCallingUid(), "caller is not the shelf");
            try {
                int uid = getPackageManager().getApplicationInfo(packageName, 0).uid;
                AppOpsManager appOps = getSystemService(AppOpsManager.class);
                appOps.setMode(AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW, uid, packageName,
                        AppOpsManager.MODE_ALLOWED);
                return appOps.checkOpNoThrow(AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW, uid,
                        packageName) == AppOpsManager.MODE_ALLOWED;
            } catch (PackageManager.NameNotFoundException e) {
                deny("ensureShellOverlayAccess", Binder.getCallingUid(),
                        "allowlisted shelf package disappeared");
                return false;
            }
        }

        @Override public String getRecentTasks(int maxTasks) {
            int uid = Binder.getCallingUid();
            enforceLauncherCaller("getRecentTasks");
            int limit = Math.max(0, Math.min(maxTasks, MAX_EXPOSED_TASKS));
            JSONArray result = new JSONArray();
            if (limit == 0) return result.toString();
            try {
                for (ActivityManager.RecentTaskInfo task : recentTasks(uid, limit)) {
                    if (task.userId != ActivityManager.getCurrentUser()) continue;
                    String packageName = taskPackageName(task);
                    if (packageName == null) continue;
                    JSONObject item = new JSONObject();
                    item.put("taskId", task.taskId);
                    item.put("packageName", packageName);
                    item.put("windowingMode", task.getWindowingMode());
                    result.put(item);
                }
                return result.toString();
            } catch (JSONException e) {
                throw new IllegalStateException("Cannot encode recent tasks", e);
            }
        }

        @Override public boolean moveTaskToFront(int taskId) {
            int uid = Binder.getCallingUid();
            enforceLauncherCaller("moveTaskToFront:" + taskId);
            if (findRecentTask(taskId, uid) == null) return false;
            try {
                ActivityTaskManager.getService().moveTaskToFront(null,
                        "org.matonos.systembridge", taskId, 0, null);
                return true;
            } catch (RemoteException | RuntimeException e) {
                Log.w(TAG, "Cannot move allowlisted recent task to front: " + taskId, e);
                return false;
            }
        }

        @Override public boolean setTaskFullscreen(int taskId) {
            int uid = Binder.getCallingUid();
            enforceLauncherCaller("setTaskFullscreen:" + taskId);
            ActivityManager.RecentTaskInfo task = findRecentTask(taskId, uid);
            if (task == null || task.token == null) return false;
            if (task.getWindowingMode() == WindowConfiguration.WINDOWING_MODE_FULLSCREEN) return true;
            try {
                WindowContainerTransaction transaction = new WindowContainerTransaction()
                        .setWindowingMode(task.token, WindowConfiguration.WINDOWING_MODE_FULLSCREEN);
                new WindowOrganizer().applyTransaction(transaction);
                return true;
            } catch (RuntimeException e) {
                Log.w(TAG, "Cannot make allowlisted recent task fullscreen: " + taskId, e);
                return false;
            }
        }

        @Override public boolean removeRecentTask(int taskId) {
            int uid = Binder.getCallingUid();
            String packageName = enforceLauncherCaller("removeRecentTask:" + taskId);
            ActivityManager.RecentTaskInfo task = findRecentTask(taskId, uid);
            if (task == null) return false;
            String taskPackage = taskPackageName(task);
            if (taskPackage == null || taskPackage.equals(packageName)
                    || isMatonLauncherPackage(taskPackage)
                    || taskPackage.equals("com.android.systemui")) return false;
            try { return ActivityTaskManager.getService().removeTask(taskId); }
            catch (RemoteException | RuntimeException e) {
                Log.w(TAG, "Cannot remove allowlisted current-user task " + taskId, e);
                return false;
            }
        }

        @Override public boolean navigateBack(boolean longPress) {
            enforceSelectedProviderCaller("nav.back", "navigateBack:" + longPress);
            return injectBack(longPress);
        }

        @Override public boolean navigateHome() {
            enforceSelectedProviderCaller("nav.home", "navigateHome");
            Intent home = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            try { startActivityAsUser(home, UserHandle.CURRENT); return true; }
            catch (RuntimeException e) { Log.e(TAG, "Authorized Home navigation failed", e); return false; }
        }

        @Override public boolean navigateRecents() {
            int uid = Binder.getCallingUid();
            enforceSelectedProviderCaller("nav.recents", "navigateRecents");
            try {
                List<ActivityManager.RecentTaskInfo> tasks = recentTasks(uid, MAX_EXPOSED_TASKS);
                if (!tasks.isEmpty() && "org.matonos.recents".equals(taskPackageName(tasks.get(0)))) {
                    for (ActivityManager.RecentTaskInfo task : tasks) {
                        String pkg = taskPackageName(task);
                        if (pkg == null || isMatonLauncherPackage(pkg) || "com.android.systemui".equals(pkg)) continue;
                        ActivityTaskManager.getService().moveTaskToFront(null,
                                "org.matonos.systembridge", task.taskId, 0, null);
                        return true;
                    }
                    return false;
                }
                Intent recents = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_DEFAULT)
                        .setComponent(new ComponentName("org.matonos.recents",
                                "org.matonos.recents.RecentsComponentAnchor"))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
                startActivityAsUser(recents, UserHandle.CURRENT);
                return true;
            } catch (RemoteException | RuntimeException e) {
                Log.e(TAG, "Authorized Recents navigation failed", e); return false;
            }
        }

        @Override public byte[] getRecentTaskThumbnail(int taskId) {
            int uid = Binder.getCallingUid();
            String caller = enforceAuthorizedCaller("launcher", "getRecentTaskThumbnail:" + taskId);
            if (!isLauncherCaller(caller)) deny("getRecentTaskThumbnail", uid,
                    "caller package is not Shell, Shelf, or Recents");
            ActivityManager.RecentTaskInfo task = findRecentTask(taskId, uid);
            String packageName = task == null ? null : taskPackageName(task);
            if (task == null || packageName == null || isMatonLauncherPackage(packageName)
                    || "com.android.systemui".equals(packageName)
                    || task.userId != ActivityManager.getCurrentUser()) return new byte[0];
            try {
                TaskSnapshot snapshot = TaskSnapshotManager.getInstance().getTaskSnapshot(
                        taskId, TaskSnapshotManager.RESOLUTION_LOW);
                if (snapshot == null || !snapshot.isBufferValid() || snapshot.hasProtectedContent()) return new byte[0];
                Bitmap source = snapshot.wrapToBitmap();
                if (source == null) return new byte[0];
                float scale = Math.min(1f, Math.min(512f / source.getWidth(), 512f / source.getHeight()));
                Bitmap scaled = Bitmap.createScaledBitmap(source,
                        Math.max(1, Math.round(source.getWidth() * scale)),
                        Math.max(1, Math.round(source.getHeight() * scale)), true);
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                for (int attempt = 0; attempt < 4; attempt++) {
                    out.reset();
                    scaled.compress(Bitmap.CompressFormat.PNG, 100, out);
                    if (out.size() <= 512 * 1024) break;
                    Bitmap smaller = Bitmap.createScaledBitmap(scaled,
                            Math.max(1, scaled.getWidth() * 3 / 4),
                            Math.max(1, scaled.getHeight() * 3 / 4), true);
                    if (smaller != scaled) scaled.recycle();
                    scaled = smaller;
                }
                if (scaled != source) scaled.recycle();
                source.recycle();
                byte[] png = out.toByteArray();
                return png.length <= 512 * 1024 ? png : new byte[0];
            } catch (Exception e) {
                Log.w(TAG, "Authorized task thumbnail unavailable taskId=" + taskId, e);
                return new byte[0];
            }
        }

        @Override public String getNavigationBarProviderState() {
            String caller = enforceAuthorizedCaller("status", "getNavigationBarProviderState");
            if (!"org.matonos.settings".equals(caller))
                deny("getNavigationBarProviderState", Binder.getCallingUid(), "Settings is required");
            return navigationProviderState().toString();
        }

        @Override public boolean setNavigationBarProvider(String packageName, boolean enabled) {
            String caller = enforceAuthorizedCaller("status", "setNavigationBarProvider:" + packageName);
            if (!"org.matonos.settings".equals(caller))
                deny("setNavigationBarProvider", Binder.getCallingUid(), "Settings is required");
            if (packageName == null || packageName.isEmpty()) {
                if (enabled) throw new IllegalArgumentException("provider package is required");
                getSharedPreferences("navigation_bar_provider", MODE_PRIVATE).edit()
                        .putBoolean("enabled", false).apply();
                Log.i(TAG, "Navigation provider explicitly disabled by user through Settings");
                applyNavigationProviderSelection();
                return true;
            }
            String certificate = certificateFor(this, packageName);
            if ("unavailable".equals(certificate) || "package unavailable".equals(certificate))
                throw new IllegalArgumentException("selected provider is not installed or has no signing certificate");
            try {
                android.content.pm.ServiceInfo service = getPackageManager().getServiceInfo(
                        new ComponentName(packageName, packageName + ".MatonNavigationBarProviderService"), 0);
                if (!service.exported) throw new IllegalArgumentException("provider service must be exported");
            } catch (PackageManager.NameNotFoundException e) {
                throw new IllegalArgumentException("selected package has no MatonOS navigation provider service", e);
            }
            getSharedPreferences("navigation_bar_provider", MODE_PRIVATE).edit()
                    .putString("package", packageName).putString("certificate", certificate)
                    .putBoolean("enabled", enabled).apply();
            Log.i(TAG, (enabled ? "Selected" : "Preset") + " navigation provider=" + packageName
                    + " certificate=" + certificate + " enabled=" + enabled
                    + " caller=" + caller);
            applyNavigationProviderSelection();
            return true;
        }

        @Override public String call(String target, String command, String jsonArgs) {
            enforceAuthorizedCaller(target, "call:" + command);
            validateName(target, "target");
            validateName(command, "command");
            try {
                if (jsonArgs != null && jsonArgs.length() > 64 * 1024)
                    throw new IllegalArgumentException("channel arguments too large");
                JSONObject args = new JSONObject(jsonArgs == null ? "{}" : jsonArgs);
                if (args.toString().length() > 64 * 1024)
                    throw new IllegalArgumentException("channel arguments too large");
                if ("input".equals(target) && "set_absolute_pointer_mode".equals(command)) {
                    return setAbsolutePointerMode(UserHandle.USER_CURRENT).toString();
                }
                IChannel channel = channelFor(target);
                if (channel == null) throw new IllegalStateException("channel instance unavailable: " + target);
                String result = channel.call(command, args.toString());
                if (result == null || result.length() > 64 * 1024)
                    throw new IllegalStateException("invalid channel result");
                org.json.JSONTokener parsed = new org.json.JSONTokener(result);
                parsed.nextValue();
                if (parsed.nextClean() != 0) throw new IllegalStateException("trailing channel result data");
                return result;
            } catch (Exception e) {
                Log.e(TAG, "Daemon call failed: target=" + target + " command=" + command, e);
                throw new IllegalStateException("Daemon call failed", e);
            }
        }

        @Override public void subscribe(String target, String topic, IMatonosListener listener) {
            enforceAuthorizedCaller(target, "subscribe:" + topic);
            validateName(target, "target");
            validateName(topic, "topic");
            if (listener == null) throw new IllegalArgumentException("listener is required");
            String key = subscriptionKey(listener.asBinder(), target, topic);
            if (subscriptions.containsKey(key)) return;
            IChannel channel = channelFor(target);
            if (channel == null) throw new IllegalStateException("channel instance unavailable: " + target);
            Subscription subscription = new Subscription(key, target, topic, listener, channel);
            Subscription previous = subscriptions.putIfAbsent(key, subscription);
            if (previous != null) return;
            try {
                channel.subscribe(topic, subscription.channelListener);
            } catch (Exception e) {
                subscriptions.remove(key, subscription);
                Log.e(TAG, "Cannot subscribe to " + target + "/" + topic, e);
                throw new IllegalStateException("Channel subscription failed", e);
            }
        }

        @Override public void unsubscribe(String target, String topic, IMatonosListener listener) {
            enforceAuthorizedCaller(target, "unsubscribe:" + topic);
            validateName(target, "target");
            validateName(topic, "topic");
            if (listener == null) return;
            Subscription subscription = subscriptions.remove(subscriptionKey(listener.asBinder(), target, topic));
            if (subscription != null) subscription.close();
        }
    };

    @Override public IBinder onBind(Intent intent) {
        // Android enforces the service's SYSTEM_BRIDGE permission before bind.
        return binder;
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            Log.i(TAG, "Absolute pointer settings applied: " +
                    setAbsolutePointerMode(UserHandle.USER_CURRENT));
        } catch (RuntimeException | JSONException e) {
            Log.e(TAG, "Cannot apply absolute pointer settings", e);
        }
        return START_STICKY;
    }

    @Override public void onCreate() {
        super.onCreate();
        navigationBarWindow = new NavigationBarWindow(this);
        applyNavigationProviderSelection();
        geometryHandler = new android.os.Handler(getMainLooper());
        DisplayManager displayManager = getSystemService(DisplayManager.class);
        if (displayManager != null) {
            displayManager.registerDisplayListener(displayListener, new android.os.Handler(getMainLooper()));
            publishDisplayGeometry();
        }
        // Absolute VM/tablet input is translated to relative motion by inputd.
        // Keep InputReader's global cursor transform linear and unit gain.
        try {
            JSONObject inputMode = setAbsolutePointerMode(UserHandle.USER_CURRENT);
            Log.i(TAG, "Absolute pointer settings applied: " + inputMode);
        } catch (RuntimeException | JSONException e) {
            Log.e(TAG, "Cannot apply absolute pointer settings", e);
        }
        // The bridge is bound explicitly by apps. Shell administration is exposed
        // through the UID-gated ContentProvider below, never ServiceManager.
    }

    private void publishDisplayGeometry() {
        DisplayManager displayManager = getSystemService(DisplayManager.class);
        Display display = displayManager == null ? null : displayManager.getDisplay(Display.DEFAULT_DISPLAY);
        if (display == null) return;
        Point size = new Point();
        display.getRealSize(size);
        if (size.x <= 0 || size.y <= 0) return;
        try {
            IChannel channel = channelFor("input");
            if (channel == null) {
                scheduleDisplayGeometryRetry();
                return;
            }
            JSONObject args = new JSONObject();
            args.put("width", size.x);
            args.put("height", size.y);
            String response = channel.call("set_display_size", args.toString());
            JSONObject result = new JSONObject(response == null ? "{}" : response);
            if (!result.optBoolean("ok", false)) {
                scheduleDisplayGeometryRetry();
                return;
            }
            Log.i(TAG, "Sent active display geometry " + size.x + "x" + size.y + " to inputd");
        } catch (Exception e) {
            Log.w(TAG, "Cannot send active display geometry to inputd; retrying", e);
            scheduleDisplayGeometryRetry();
        }
    }

    private void scheduleDisplayGeometryRetry() {
        if (geometryRetryPending) return;
        geometryRetryPending = true;
        geometryHandler.postDelayed(() -> {
            geometryRetryPending = false;
            publishDisplayGeometry();
        }, 1000);
    }

    private JSONObject setAbsolutePointerMode(int userId) throws JSONException {
        AppOpsManager appOps = getSystemService(AppOpsManager.class);
        appOps.setMode(AppOpsManager.OPSTR_WRITE_SETTINGS, android.os.Process.myUid(),
                getPackageName(), AppOpsManager.MODE_ALLOWED);
        boolean accelerationWritten = Settings.System.putIntForUser(getContentResolver(),
                Settings.System.MOUSE_POINTER_ACCELERATION_ENABLED, 0, userId);
        boolean speedWritten = Settings.System.putIntForUser(getContentResolver(),
                Settings.System.POINTER_SPEED, -7, userId);
        if (!accelerationWritten || !speedWritten)
            throw new IllegalStateException("SettingsProvider rejected absolute pointer mode");
        JSONObject result = new JSONObject();
        result.put("accelerationEnabled", Settings.System.getIntForUser(getContentResolver(),
                Settings.System.MOUSE_POINTER_ACCELERATION_ENABLED, 1, userId) != 0);
        result.put("pointerSpeed", Settings.System.getIntForUser(getContentResolver(),
                Settings.System.POINTER_SPEED, 0, userId));
        return result;
    }

    private JSONObject navigationProviderState() {
        android.content.SharedPreferences prefs = getSharedPreferences(
                "navigation_bar_provider", MODE_PRIVATE);
        JSONObject state = new JSONObject();
        try {
            state.put("packageName", prefs.getString("package", NavigationBarWindow.DEFAULT_PROVIDER));
            state.put("enabled", prefs.getBoolean("enabled", false));
            state.put("certificateSha256", prefs.getString("certificate", ""));
            state.put("defaultDenied", !prefs.getBoolean("enabled", false));
        } catch (JSONException impossible) { }
        return state;
    }

    private void applyNavigationProviderSelection() {
        if (navigationBarWindow == null) return;
        android.content.SharedPreferences prefs = getSharedPreferences(
                "navigation_bar_provider", MODE_PRIVATE);
        String pkg = prefs.getString("package", NavigationBarWindow.DEFAULT_PROVIDER);
        String cert = prefs.getString("certificate", null);
        boolean enabled = prefs.getBoolean("enabled", false);
        navigationBarWindow.select(pkg, cert, enabled);
    }

    private String enforceSelectedProviderCaller(String target, String operation) {
        int uid = Binder.getCallingUid();
        android.content.SharedPreferences prefs = getSharedPreferences(
                "navigation_bar_provider", MODE_PRIVATE);
        String selected = prefs.getString("package", NavigationBarWindow.DEFAULT_PROVIDER);
        String pinnedCertificate = prefs.getString("certificate", null);
        if (!prefs.getBoolean("enabled", false) || pinnedCertificate == null)
            deny(operation, uid, "navigation provider capability is not enabled by the user");
        String[] packages = getPackageManager().getPackagesForUid(uid);
        if (packages != null) for (String pkg : packages) {
            if (!selected.equals(pkg)) continue;
            String current = certificateFor(this, pkg);
            if (!pinnedCertificate.equals(current)) break;
            Log.i(TAG, "Authorized provider action=" + operation + " target=" + target
                    + " package=" + pkg + " uid=" + uid);
            return pkg;
        }
        deny(operation, uid, "caller is not the selected, certificate-pinned navigation provider");
        return null;
    }

    static String certificateFor(android.content.Context context, String packageName) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.signingInfo == null || info.signingInfo.getApkContentsSigners().length != 1)
                return "unavailable";
            return sha256(info.signingInfo.getApkContentsSigners()[0].toByteArray());
        } catch (PackageManager.NameNotFoundException e) {
            return "package unavailable";
        }
    }

    private void enforcePermission(int uid, String operation) {
        if (checkPermission(PERMISSION, -1, uid) != PackageManager.PERMISSION_GRANTED)
            deny(operation, uid, "missing permission");
    }

    private String enforceAuthorizedCaller(String target, String operation) {
        int uid = Binder.getCallingUid();
        enforcePermission(uid, operation);
        enforceNotBanned(uid, operation);
        String packageName = authorizedPackage(uid, target);
        if (packageName != null) {
            Log.i(TAG, "Authorized action=" + operation + " target=" + target
                    + " package=" + packageName + " uid=" + uid);
            return packageName;
        }
        deny(operation, uid, "package or signing certificate not allowlisted for target=" + target);
        return null;
    }

    private void enforceNotBanned(int uid, String operation) {
        String[] packages = getPackageManager().getPackagesForUid(uid);
        if (packages == null) return;
        for (String packageName : packages) try {
            PackageInfo info = getPackageManager().getPackageInfo(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.signingInfo != null && info.signingInfo.getApkContentsSigners().length > 0
                    && isBanned(this, packageName, sha256(
                            info.signingInfo.getApkContentsSigners()[0].toByteArray()))) {
                Log.w(TAG, "Denied banned caller package=" + packageName + " operation=" + operation);
                throw new SecurityException("BRIDGE_NOT_INSTALLED");
            }
        } catch (PackageManager.NameNotFoundException ignored) { }
    }

    private String authorizedPackage(int uid, String target) {
        Set<String> certs = allowedCerts();
        Set<String> packagesForTarget = targetCallers();
        String[] packages = getPackageManager().getPackagesForUid(uid);
        if (packages != null) {
            for (String packageName : packages) {
                try {
                    PackageInfo info = getPackageManager().getPackageInfo(packageName,
                            PackageManager.GET_SIGNING_CERTIFICATES);
                    if (info.signingInfo == null) continue;
                    String cert = sha256(info.signingInfo.getApkContentsSigners()[0].toByteArray());
                    if (isBanned(this, packageName, cert)) {
                        Log.w(TAG, "Denied banned package=" + packageName + " target=" + target);
                        continue;
                    }
                    boolean builtin = packagesForTarget.contains(target + " " + packageName)
                            && certs.contains(cert);
                    boolean trusted = trustedTargets(packageName, cert).contains(target);
                    if (!builtin && !trusted) continue;
                    return packageName;
                } catch (PackageManager.NameNotFoundException ignored) {
                    // Package disappeared between uid lookup and certificate check.
                }
            }
        }
        return null;
    }

    private Set<String> trustedTargets(String packageName, String cert) {
        String json = getSharedPreferences("trusted_developer_apps", MODE_PRIVATE)
                .getString(packageName, null);
        if (json == null) return java.util.Collections.emptySet();
        try {
            JSONObject entry = new JSONObject(json);
            if (!cert.equalsIgnoreCase(entry.getString("certSha256"))) return java.util.Collections.emptySet();
            JSONArray values = entry.getJSONArray("targets");
            Set<String> result = new HashSet<>();
            for (int i = 0; i < values.length(); i++) result.add(values.getString(i));
            return result;
        } catch (Exception e) { return java.util.Collections.emptySet(); }
    }

    private static boolean isBanned(android.content.Context context, String packageName, String cert) {
        String key = packageName.toLowerCase(Locale.ROOT);
        android.content.SharedPreferences overrides = context.getSharedPreferences(
                "bridge_ban_overrides", MODE_PRIVATE);
        if (overrides.contains(key)) return overrides.getBoolean(key, false);
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream("/odm/etc/matonos/bridge-banlist.txt"), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim().toLowerCase(Locale.ROOT);
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] fields = line.split("\\s+", 2);
                if (fields[0].equals(key) && (fields.length == 1 || fields[1].equals(cert))) return true;
            }
        } catch (Exception ignored) { }
        return false;
    }

    private static Set<String> knownTargets() {
        return new HashSet<>(java.util.Arrays.asList("launcher", "input", "sleep", "wifi",
                "bluetooth", "audio", "camera", "status", "nav.back", "nav.home", "nav.recents"));
    }

    private static void grantTrust(android.content.Context context, String packageName, Set<String> targets) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.signingInfo == null || info.signingInfo.getApkContentsSigners().length != 1)
                throw new IllegalArgumentException("package must have one current signing certificate");
            String cert = sha256(info.signingInfo.getApkContentsSigners()[0].toByteArray());
            if (isBanned(context, packageName, cert))
                throw new SecurityException("package is blocked from MatonOS bridge access");
            JSONArray values = new JSONArray();
            for (String target : targets) values.put(target);
            JSONObject entry = new JSONObject(); entry.put("certSha256", cert); entry.put("targets", values);
            context.getPackageManager().grantRuntimePermission(packageName, PERMISSION, UserHandle.getUserHandleForUid(info.applicationInfo.uid));
            context.getSharedPreferences("trusted_developer_apps", android.content.Context.MODE_PRIVATE).edit().putString(packageName, entry.toString()).apply();
            Log.i(TAG, "Developer trust granted for " + packageName + " cert=" + cert + " targets=" + targets);
        } catch (Exception e) {
            Log.w(TAG, "Developer trust grant denied for " + packageName, e);
            throw new IllegalArgumentException("cannot trust " + packageName + ": " + e.getMessage());
        }
    }

    private static void revokePermissionUnlessBuiltin(android.content.Context context,
                                                      String packageName, int uid, String cert) {
        boolean builtin = false;
        for (String entry : readLines(context, R.raw.target_caller_allowlist)) {
            if (!entry.endsWith(" " + packageName)) continue;
            if (readLines(context, R.raw.caller_cert_allowlist).contains(cert)) { builtin = true; break; }
        }
        if (!builtin) context.getPackageManager().revokeRuntimePermission(packageName, PERMISSION,
                UserHandle.getUserHandleForUid(uid));
    }

    public static final class TrustedAppsActivity extends android.app.Activity {
        private boolean replied;
        @Override protected void onCreate(android.os.Bundle state) {
            super.onCreate(state);
            String pkg = getIntent().getStringExtra("requestedPackage");
            String[] requested = getIntent().getStringArrayExtra("requestedTargets");
            if (pkg == null) pkg = "";
            final String packageName = pkg;
            if (!packageName.isEmpty()) {
                String requester = getCallingPackage();
                if (!packageName.equals(requester)) {
                    reply("UNAVAILABLE", "BRIDGE_NOT_INSTALLED");
                    finish();
                    return;
                }
                String cert = certificateFor(packageName);
                if (!"unavailable".equals(cert) && !"package unavailable".equals(cert)
                        && isBanned(this, packageName, cert)) {
                    reply("UNAVAILABLE", "BRIDGE_NOT_INSTALLED");
                    finish();
                    return;
                }
            }
            final Set<String> targets = new HashSet<>();
            if (requested != null) for (String target : requested) if (knownTargets().contains(target)) targets.add(target);
            boolean requestEnabled = Settings.Global.getInt(getContentResolver(),
                    Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) == 1
                    && Settings.Global.getInt(getContentResolver(), "matonos_allow_bridge_trust_requests", 0) == 1;
            android.widget.LinearLayout layout = new android.widget.LinearLayout(this);
            layout.setOrientation(android.widget.LinearLayout.VERTICAL);
            layout.setPadding(32, 32, 32, 32);
            android.widget.CheckBox requests = new android.widget.CheckBox(this);
            requests.setText("Allow apps to request bridge trust");
            requests.setChecked(Settings.Global.getInt(getContentResolver(), "matonos_allow_bridge_trust_requests", 0) == 1);
            requests.setVisibility(Settings.Global.getInt(getContentResolver(), Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) == 1
                    ? android.view.View.VISIBLE : android.view.View.GONE);
            requests.setOnCheckedChangeListener((button, checked) -> Settings.Global.putInt(getContentResolver(),
                    "matonos_allow_bridge_trust_requests", checked ? 1 : 0));
            layout.addView(requests);
            android.widget.TextView intro = new android.widget.TextView(this);
            intro.setText("Trusted developer apps\n\nTrust grants bridge access to the selected MatonOS services. It can be revoked here at any time.");
            layout.addView(intro);
            android.content.SharedPreferences banOverrides = getSharedPreferences(
                    "bridge_ban_overrides", MODE_PRIVATE);
            StringBuilder blockedList = new StringBuilder("\nUser bridge access rules\n");
            for (Map.Entry<String, ?> entry : banOverrides.getAll().entrySet()) {
                blockedList.append(entry.getKey()).append(entry.getValue().equals(Boolean.TRUE)
                        ? " — blocked" : " — allowed (default override)").append('\n');
            }
            android.widget.TextView bans = new android.widget.TextView(this);
            bans.setText(blockedList.toString());
            layout.addView(bans);
            android.widget.EditText blockedPackage = new android.widget.EditText(this);
            blockedPackage.setSingleLine(true);
            blockedPackage.setHint("Package to block or unblock");
            layout.addView(blockedPackage);
            android.widget.Button block = new android.widget.Button(this);
            block.setText("Block bridge access");
            block.setOnClickListener(v -> {
                String name = blockedPackage.getText().toString().trim();
                if (!name.isEmpty()) getSharedPreferences("bridge_ban_overrides", MODE_PRIVATE)
                        .edit().putBoolean(name.toLowerCase(Locale.ROOT), true).apply();
                Log.i(TAG, "User blocked bridge access for " + name);
                finish(); startActivity(new Intent(this, TrustedAppsActivity.class));
            });
            layout.addView(block);
            android.widget.Button unblock = new android.widget.Button(this);
            unblock.setText("Unblock / override default");
            unblock.setOnClickListener(v -> {
                String name = blockedPackage.getText().toString().trim();
                if (!name.isEmpty()) getSharedPreferences("bridge_ban_overrides", MODE_PRIVATE)
                        .edit().putBoolean(name.toLowerCase(Locale.ROOT), false).apply();
                Log.i(TAG, "User allowed bridge access override for " + name);
                finish(); startActivity(new Intent(this, TrustedAppsActivity.class));
            });
            layout.addView(unblock);
            if (!packageName.isEmpty()) {
                android.widget.TextView candidate = new android.widget.TextView(this);
                candidate.setText("\nRequest from " + packageName + "\nCertificate SHA-256: "
                        + certificateFor(packageName) + "\nTargets: " + targets);
                layout.addView(candidate);
                android.widget.Button allow = new android.widget.Button(this); allow.setText("Trust app");
                allow.setEnabled(requestEnabled && !targets.isEmpty());
                allow.setOnClickListener(v -> {
                    try { grantTrust(this, packageName, targets); reply("GRANTED", null); finish(); }
                    catch (Exception e) { reply("DENIED", e.getMessage()); finish(); }
                }); layout.addView(allow);
                android.widget.Button deny = new android.widget.Button(this); deny.setText("Deny");
                deny.setOnClickListener(v -> { reply("DENIED", "USER_DENIED"); finish(); }); layout.addView(deny);
            }
            for (String trusted : getSharedPreferences("trusted_developer_apps", MODE_PRIVATE).getAll().keySet()) {
                android.widget.Button revoke = new android.widget.Button(this); revoke.setText("Revoke " + trusted);
                revoke.setOnClickListener(v -> {
                    getSharedPreferences("trusted_developer_apps", MODE_PRIVATE).edit().remove(trusted).apply();
                    try {
                        PackageInfo packageInfo = getPackageManager().getPackageInfo(trusted, PackageManager.GET_SIGNING_CERTIFICATES);
                        String cert = packageInfo.signingInfo.getApkContentsSigners().length == 1
                                ? sha256(packageInfo.signingInfo.getApkContentsSigners()[0].toByteArray()) : "";
                        revokePermissionUnlessBuiltin(this, trusted, packageInfo.applicationInfo.uid, cert);
                    }
                    catch (Exception ignored) { }
                    finish(); startActivity(new Intent(this, TrustedAppsActivity.class));
                }); layout.addView(revoke);
            }
            setContentView(layout);
        }
        private void reply(String status, String reason) {
            replied = true;
            String action = getIntent().getStringExtra("callbackAction");
            String target = getIntent().getStringExtra("callbackPackage");
            if (action == null || target == null) return;
            Intent result = new Intent(action).setPackage(target)
                    .putExtra("callbackNonce", getIntent().getStringExtra("callbackNonce"))
                    .putExtra("trustStatus", status).putExtra("trustReason", reason);
            sendBroadcast(result);
        }
        private String certificateFor(String pkg) {
            try {
                PackageInfo info = getPackageManager().getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
                if (info.signingInfo == null || info.signingInfo.getApkContentsSigners().length != 1) return "unavailable";
                return sha256(info.signingInfo.getApkContentsSigners()[0].toByteArray());
            } catch (Exception e) { return "package unavailable"; }
        }
        @Override protected void onDestroy() {
            if (!replied && getIntent().hasExtra("callbackAction")) reply("DENIED", "USER_DENIED");
            super.onDestroy();
        }
    }

    /** adb content-call endpoint; only shell and root may administer developer trust. */
    public static final class ShellProvider extends android.content.ContentProvider {
        @Override public boolean onCreate() { return true; }
        @Override public android.os.Bundle call(String method, String arg, android.os.Bundle extras) {
            int uid = Binder.getCallingUid();
            if (uid != 0 && uid != 2000) {
                Log.w(TAG, "Denied bridge trust provider method=" + method + " uid=" + uid);
                throw new SecurityException("Only shell or root may administer bridge trust");
            }
            android.os.Bundle result = new android.os.Bundle();
            try {
                if ("list".equals(method)) {
                    result.putString("result", getContext().getSharedPreferences(
                            "trusted_developer_apps", android.content.Context.MODE_PRIVATE).getAll().toString());
                } else if ("trust".equals(method)) {
                    String targetsText = extras == null ? null : extras.getString("targets");
                    Set<String> targets = new HashSet<>();
                    if (targetsText != null) for (String target : targetsText.split(",")) {
                        target = target.trim();
                        if (!knownTargets().contains(target))
                            throw new IllegalArgumentException("unknown target " + target);
                        targets.add(target);
                    }
                    if (arg == null || targets.isEmpty())
                        throw new IllegalArgumentException("trust requires package and targets");
                    if (Settings.Global.getInt(getContext().getContentResolver(),
                            Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) != 1)
                        throw new SecurityException("enable Developer options first");
                    grantTrust(getContext(), arg, targets);
                    result.putString("result", "trusted " + arg + " for " + targets);
                } else if ("untrust".equals(method)) {
                    if (arg == null || arg.isEmpty()) throw new IllegalArgumentException("package required");
                    getContext().getSharedPreferences("trusted_developer_apps", android.content.Context.MODE_PRIVATE)
                            .edit().remove(arg).apply();
                    try {
                        PackageInfo info = getContext().getPackageManager().getPackageInfo(arg,
                                PackageManager.GET_SIGNING_CERTIFICATES);
                        String cert = info.signingInfo == null ? "" : sha256(
                                info.signingInfo.getApkContentsSigners()[0].toByteArray());
                        revokePermissionUnlessBuiltin(getContext(), arg, info.applicationInfo.uid, cert);
                    } catch (PackageManager.NameNotFoundException ignored) { }
                    Log.i(TAG, "Developer trust revoked for " + arg + " by uid=" + uid);
                    result.putString("result", "revoked " + arg);
                } else if ("ban".equals(method) || "unban".equals(method)) {
                    if (arg == null || !arg.matches("[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)+"))
                        throw new IllegalArgumentException("valid package name required");
                    boolean banned = "ban".equals(method);
                    getContext().getSharedPreferences("bridge_ban_overrides", android.content.Context.MODE_PRIVATE)
                            .edit().putBoolean(arg.toLowerCase(Locale.ROOT), banned).apply();
                    Log.i(TAG, "User " + (banned ? "blocked" : "unblocked")
                            + " bridge access for " + arg + " by uid=" + uid);
                    result.putString("result", (banned ? "blocked " : "unblocked ") + arg);
                } else throw new IllegalArgumentException("method must be trust, untrust, or list");
                return result;
            } catch (RuntimeException e) {
                Log.w(TAG, "Bridge trust provider request failed method=" + method
                        + " package=" + arg + " uid=" + uid, e);
                throw e;
            }
        }
        @Override public android.database.Cursor query(android.net.Uri uri, String[] projection,
                String selection, String[] selectionArgs, String sortOrder) { return null; }
        @Override public String getType(android.net.Uri uri) { return null; }
        @Override public android.net.Uri insert(android.net.Uri uri, android.content.ContentValues values) { return null; }
        @Override public int delete(android.net.Uri uri, String selection, String[] selectionArgs) { return 0; }
        @Override public int update(android.net.Uri uri, android.content.ContentValues values,
                String selection, String[] selectionArgs) { return 0; }
    }

    private List<ActivityManager.RecentTaskInfo> recentTasks(int callingUid, int limit) {
        int userId = UserHandle.getUserId(callingUid);
        if (userId != ActivityManager.getCurrentUser()) return java.util.Collections.emptyList();
        return ActivityTaskManager.getInstance().getRecentTasks(limit,
                ActivityManager.RECENT_IGNORE_UNAVAILABLE | ActivityManager.RECENT_WITH_EXCLUDED,
                userId);
    }

    private ActivityManager.RecentTaskInfo findRecentTask(int taskId, int callingUid) {
        for (ActivityManager.RecentTaskInfo task : recentTasks(callingUid, MAX_EXPOSED_TASKS)) {
            if (task.taskId == taskId && task.userId == ActivityManager.getCurrentUser()) return task;
        }
        return null;
    }

    private static String taskPackageName(ActivityManager.RecentTaskInfo task) {
        ComponentName component = task.topActivity != null ? task.topActivity
                : task.baseActivity != null ? task.baseActivity
                : task.origActivity != null ? task.origActivity : task.realActivity;
        if (component != null) return component.getPackageName();
        if (task.baseIntent != null && task.baseIntent.getComponent() != null)
            return task.baseIntent.getComponent().getPackageName();
        return null;
    }

    private static boolean isMatonLauncherPackage(String packageName) {
        return "org.matonos.shell".equals(packageName) || "org.matonos.shelf".equals(packageName)
                || "org.matonos.recents".equals(packageName);
    }

    private static boolean isLauncherCaller(String packageName) {
        return isMatonLauncherPackage(packageName);
    }

    private String enforceLauncherCaller(String operation) {
        String packageName = enforceAuthorizedCaller("launcher", operation);
        if (!isLauncherCaller(packageName))
            deny(operation, Binder.getCallingUid(), "caller must be MatonOS Shell, Shelf, or Recents");
        return packageName;
    }

    private void requireShelfCaller(String packageName, String operation) {
        if (!"org.matonos.shelf".equals(packageName))
            deny(operation, Binder.getCallingUid(), "only MatonOS Shelf may navigate");
    }

    private boolean injectBack(boolean longPress) {
        InputManager input = getSystemService(InputManager.class);
        long downTime = SystemClock.uptimeMillis();
        KeyEvent down = new KeyEvent(downTime, downTime, KeyEvent.ACTION_DOWN,
                KeyEvent.KEYCODE_BACK, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
                KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD);
        boolean first = input.injectInputEvent(down, InputManager.INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH);
        if (longPress && first) {
            try { Thread.sleep(600); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            KeyEvent repeat = new KeyEvent(downTime, SystemClock.uptimeMillis(), KeyEvent.ACTION_DOWN,
                    KeyEvent.KEYCODE_BACK, 1, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
                    KeyEvent.FLAG_FROM_SYSTEM | KeyEvent.FLAG_LONG_PRESS,
                    InputDevice.SOURCE_KEYBOARD);
            first = input.injectInputEvent(repeat, InputManager.INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH) && first;
        }
        KeyEvent up = new KeyEvent(downTime, SystemClock.uptimeMillis(), KeyEvent.ACTION_UP,
                KeyEvent.KEYCODE_BACK, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
                KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD);
        boolean second = input.injectInputEvent(up, InputManager.INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH);
        return first && second;
    }

    private void deny(String operation, int uid, String reason) {
        Log.w(TAG, "Denied " + operation + " for uid=" + uid + ": " + reason);
        throw new SecurityException("MatonOS system bridge access denied");
    }

    private Set<String> allowedCerts() {
        Set<String> result = allowedCerts;
        if (result != null) return result;
        synchronized (this) {
            if (allowedCerts == null) allowedCerts = readLines(R.raw.caller_cert_allowlist);
            return allowedCerts;
        }
    }

    private Set<String> targetCallers() {
        Set<String> result = targetCallers;
        if (result != null) return result;
        synchronized (this) {
            if (targetCallers == null) targetCallers = readLines(R.raw.target_caller_allowlist);
            return targetCallers;
        }
    }

    private Set<String> readLines(int resource) { return readLines(this, resource); }

    private static Set<String> readLines(android.content.Context context, int resource) {
        Set<String> result = new HashSet<>();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                context.getResources().openRawResource(resource), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                result.add(line.toLowerCase(Locale.ROOT));
            }
        } catch (Exception e) {
            Log.e(TAG, "Cannot load bridge allowlist resource " + resource, e);
        }
        return result;
    }

    private IChannel channelFor(String target) {
        // checkService is deliberately nonblocking: optional hardware instances may be absent.
        IBinder service = ServiceManager.checkService("vendor.matonos.channel.IChannel/" + target);
        return IChannel.Stub.asInterface(service);
    }

    private final class Subscription {
        final String key;
        final String target;
        final String topic;
        final IMatonosListener appListener;
        final IChannel channel;
        final IChannelListener channelListener;
        Subscription(String key, String target, String topic, IMatonosListener listener, IChannel channel) {
            this.key = key; this.target = target; this.topic = topic;
            this.appListener = listener; this.channel = channel;
            this.channelListener = new IChannelListener.Stub() {
                @Override public int getInterfaceVersion() { return IChannelListener.VERSION; }
                @Override public String getInterfaceHash() { return IChannelListener.HASH; }
                @Override public void onEvent(String eventTopic, String json) {
                    if (!topic.equals(eventTopic) || json == null || json.length() > 64 * 1024) return;
                    try {
                        appListener.onEvent(target, eventTopic, json);
                    } catch (RemoteException e) {
                        Log.w(TAG, "App listener died for " + target + "/" + topic, e);
                        close();
                    }
                }
            };
        }
        void close() {
            subscriptions.remove(key, this);
            try { channel.unsubscribe(topic, channelListener); }
            catch (RemoteException e) { Log.w(TAG, "Channel unsubscribe failed", e); }
        }
    }

    private static String subscriptionKey(IBinder binder, String target, String topic) {
        return System.identityHashCode(binder) + ":" + target + ":" + topic;
    }

    private static void validateName(String value, String field) {
        if (value == null || !value.matches("[a-z][a-z0-9_-]{0,31}"))
            throw new IllegalArgumentException("invalid " + field);
    }

    private static String sha256(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder result = new StringBuilder(digest.length * 2);
            for (byte b : digest) result.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            return result.toString();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }
}
