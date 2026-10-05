package org.matonos.systembridge;

import android.app.ActivityManager;
import android.app.ActivityTaskManager;
import android.app.AppOpsManager;
import android.app.WindowConfiguration;
import android.app.Service;
import android.hardware.display.DisplayManager;
import android.content.Context;
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
import android.os.Build;
import android.os.ParcelFileDescriptor;
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
import java.util.Base64;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

import vendor.matonos.channel.IChannel;
import vendor.matonos.channel.IChannelListener;
import org.matonos.systembridge.ILinuxd;
import org.matonos.systembridge.ILinuxdListener;

public final class SystemBridgeService extends Service {
    private static final String TAG = "MatonSystemBridge";
    private static final String PERMISSION = "org.matonos.permission.SYSTEM_BRIDGE";
    private static final String RUNTIMES_PACKAGE = "org.matonos.linuxruntimes";
    private static final int MAX_EXPOSED_TASKS = 50;
    private static volatile SystemBridgeService activeService;
    private volatile Set<String> allowedCerts;
    private volatile Set<String> targetCallers;
    private android.os.Handler geometryHandler;
    private FlatpakStubManager flatpakStubManager;
    private boolean geometryRetryPending;
    private final ConcurrentMap<String, Subscription> subscriptions = new ConcurrentHashMap<>();
    private final ConcurrentMap<String, FlatpakSubscription> flatpakSubscriptions = new ConcurrentHashMap<>();
    private NavigationBarWindow navigationBarWindow;
    // sleep: forward Android's current suspend blockers to the sleep daemon.
    private AndroidWakeStateForwarder sleepWakeForwarder;
    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int displayId) { publishDisplayGeometry(); }
        @Override public void onDisplayRemoved(int displayId) { publishDisplayGeometry(); }
        @Override public void onDisplayChanged(int displayId) { publishDisplayGeometry(); }
    };

    private final ISystemBridge.Stub binder = new ISystemBridge.Stub() {
        @Override public String getFlatpakLaunchStatus(String ref) {
            String caller=enforceAuthorizedCaller("flatpak_launch","getFlatpakLaunchStatus");
            if (!"org.matonos.compositor".equals(caller)) throw new SecurityException("Only the compositor may inspect launches");
            long identity=Binder.clearCallingIdentity();
            try {
                ILinuxd daemon=ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
                if(daemon==null)throw new IllegalStateException("Flatpak service unavailable");
                return daemon.call("launch_status",new org.json.JSONObject().put("ref",ref).toString());
            } catch(Exception error) { throw new IllegalStateException("Cannot inspect application",error); }
            finally { Binder.restoreCallingIdentity(identity); }
        }

        @Override public String launchFlatpak(String ref, android.os.ParcelFileDescriptor runtimeDirectory, android.os.ParcelFileDescriptor x11Directory, String x11Display) {
            throw new SecurityException("A signed stub process and lifeline are required");
        }

        @Override public String launchOwnedFlatpak(String ref, android.os.ParcelFileDescriptor runtimeDirectory, android.os.ParcelFileDescriptor x11Directory, String x11Display, String dnsForwarder, int stubUid, int stubPid, android.os.ParcelFileDescriptor lifeline) {
            try {
                String caller = enforceAuthorizedCaller("flatpak_launch", "launchFlatpak");
                if (!"org.matonos.compositor".equals(caller) || runtimeDirectory == null)
                    throw new SecurityException("Only the compositor may launch graphical Flatpaks");
                // Accept only a socket name inside the delegated X11 directory.
                if (x11Display != null && x11Display.length() > 0 &&
                        (!x11Display.matches("X[0-9]+") || x11Directory == null))
                    throw new SecurityException("Invalid compositor X11 socket capability");
                if (lifeline == null || stubPid <= 0 || stubUid < 10000 ||
                        !flatpakStubManager.ownsStub(stubUid, ref))
                    throw new SecurityException("Unverified stub owner");
                long identity = Binder.clearCallingIdentity();
                try {
                    ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
                    if (daemon == null) throw new IllegalStateException("Flatpak service is unavailable");
                    String expectedDns = String.format(Locale.ROOT, "127.%d.%d.%d:53",
                            10 + ((stubUid >>> 16) & 0x3f), (stubUid >>> 8) & 0xff, stubUid & 0xff);
                    if (!expectedDns.equals(dnsForwarder))
                        throw new SecurityException("Per-app DNS endpoint is missing or does not match the stub UID");
                    return daemon.launchGraphical(ref, runtimeDirectory, expectedDns.substring(0, expectedDns.length() - 3), x11Directory, x11Display,
                            flatpakStubManager.hasGameControllers(ref), stubUid, stubPid, lifeline);
                } finally {
                    Binder.restoreCallingIdentity(identity);
                }
            } catch (RemoteException e) {
                // Server-side visibility: the client only sees the exception
                // class, never this stack.
                Log.e(TAG, "launchFlatpak failed for " + ref, e);
                throw new IllegalStateException("Flatpak service disconnected", e);
            } catch (RuntimeException error) {
                Log.e(TAG, "launchFlatpak failed for " + ref, error);
                throw error;
            } finally {
                if (lifeline != null) try { lifeline.close(); } catch (java.io.IOException ignored) { }
                if (runtimeDirectory != null) try { runtimeDirectory.close(); } catch (java.io.IOException ignored) { }
                if (x11Directory != null) try { x11Directory.close(); } catch (java.io.IOException ignored) { }
            }
        }

        @Override public boolean isFlatpakStub(int uid, String ref) {
            String caller=enforceAuthorizedCaller("flatpak_launch","isFlatpakStub");
            if(!"org.matonos.compositor".equals(caller))throw new SecurityException("Compositor only");
            long identity=Binder.clearCallingIdentity();
            try{return flatpakStubManager!=null&&flatpakStubManager.ownsStub(uid,ref);}
            finally{Binder.restoreCallingIdentity(identity);}
        }

        @Override public ParcelFileDescriptor[] createDnsForwarderSockets(String address, int stubUid, String ref) {
            String caller = enforceAuthorizedCaller("flatpak_launch", "createDnsForwarderSockets");
            if (!"org.matonos.compositor".equals(caller) || stubUid < 10000 ||
                    !flatpakStubManager.ownsStub(stubUid, ref))
                throw new SecurityException("Only the compositor may bind DNS sockets for a verified stub");
            String expected = String.format(Locale.ROOT, "127.%d.%d.%d",
                    10 + ((stubUid >>> 16) & 0x3f), (stubUid >>> 8) & 0xff, stubUid & 0xff);
            if (!expected.equals(address)) throw new SecurityException("DNS address does not match the stub UID");
            long identity = Binder.clearCallingIdentity();
            try {
                ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
                if (daemon == null) throw new IllegalStateException("linuxd is unavailable");
                return daemon.createDnsForwarderSockets(address, stubUid);
            } catch (RemoteException error) {
                throw new IllegalStateException("linuxd could not bind per-app DNS sockets", error);
            } finally {
                Binder.restoreCallingIdentity(identity);
            }
        }

        @Override public String getFlatpakStubCommits(int uid, String ref) {
            // linuxd runs as system; app callers must pass the existing scoped gate.
            if (Binder.getCallingUid() != android.os.Process.SYSTEM_UID)
                enforceAuthorizedCaller("flatpak_launch", "getFlatpakStubCommits");
            else Log.i(TAG, "Authorized action=getFlatpakStubCommits caller=system stubUid=" + uid);
            long identity = Binder.clearCallingIdentity();
            try { return flatpakStubManager.stubCommits(uid, ref); }
            catch (Exception error) { throw new IllegalStateException("Cannot resolve stub commits", error); }
            finally { Binder.restoreCallingIdentity(identity); }
        }

        @Override public int getBridgeApiVersion() {
            enforceNotBanned(Binder.getCallingUid(), "getBridgeApiVersion");
            return 8;
        }
        @Override public String getBridgeApiHash() {
            enforceNotBanned(Binder.getCallingUid(), "getBridgeApiHash");
            return "d4a82b10f6e9c73b81a2d5e4f09c8a3b7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a1";
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
                if (channelAvailable(target) && authorizedPackage(uid, target) != null) channels.put(target);
            }
            if (allowed.length() == 0) Log.w(TAG, "Denied getBridgeStatus for uid=" + uid
                    + ": no built-in or developer trust entry matches caller");
            JSONObject status = new JSONObject();
            try { status.put("apiVersion", 6); status.put("allowedTargets", allowed);
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
            // At most one source, one initial scaled bitmap, and four smaller
            // bitmaps can be allocated. Track them all so failures anywhere in
            // scaling or compression still release every native pixel buffer.
            Bitmap[] ownedBitmaps = new Bitmap[6];
            int ownedBitmapCount = 0;
            Bitmap source = null;
            Bitmap scaled = null;
            try {
                TaskSnapshot snapshot = TaskSnapshotManager.getInstance().getTaskSnapshot(
                        taskId, TaskSnapshotManager.RESOLUTION_LOW);
                if (snapshot == null || !snapshot.isBufferValid() || snapshot.hasProtectedContent()) return new byte[0];
                source = snapshot.wrapToBitmap();
                if (source == null) return new byte[0];
                ownedBitmaps[ownedBitmapCount++] = source;
                float scale = Math.min(1f, Math.min(512f / source.getWidth(), 512f / source.getHeight()));
                scaled = Bitmap.createScaledBitmap(source,
                        Math.max(1, Math.round(source.getWidth() * scale)),
                        Math.max(1, Math.round(source.getHeight() * scale)), true);
                if (scaled != source) ownedBitmaps[ownedBitmapCount++] = scaled;
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                for (int attempt = 0; attempt < 4; attempt++) {
                    out.reset();
                    scaled.compress(Bitmap.CompressFormat.PNG, 100, out);
                    if (out.size() <= 512 * 1024) break;
                    Bitmap smaller = Bitmap.createScaledBitmap(scaled,
                            Math.max(1, scaled.getWidth() * 3 / 4),
                            Math.max(1, scaled.getHeight() * 3 / 4), true);
                    if (smaller != scaled && smaller != source)
                        ownedBitmaps[ownedBitmapCount++] = smaller;
                    Bitmap previous = scaled;
                    scaled = smaller;
                    if (previous != scaled && !previous.isRecycled()) previous.recycle();
                }
                byte[] png = out.toByteArray();
                return png.length <= 512 * 1024 ? png : new byte[0];
            } catch (Exception e) {
                Log.w(TAG, "Authorized task thumbnail unavailable taskId=" + taskId, e);
                return new byte[0];
            } finally {
                for (int i = 0; i < ownedBitmapCount; i++) {
                    Bitmap bitmap = ownedBitmaps[i];
                    try {
                        if (bitmap != null && !bitmap.isRecycled()) bitmap.recycle();
                    } catch (RuntimeException e) {
                        Log.w(TAG, "Could not recycle task thumbnail bitmap", e);
                    }
                }
            }
        }

        @Override public String getNavigationBarProviderState() {
            String caller = enforceAuthorizedCaller("status", "getNavigationBarProviderState");
            if (!"org.matonos.settings".equals(caller))
                deny("getNavigationBarProviderState", Binder.getCallingUid(), "Settings is required");
            return navigationProviderState().toString();
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
                String result;
                if ("flatpak".equals(target)) {
                    ILinuxd service = linuxdFor();
                    if (service == null) throw new IllegalStateException("Flatpak service is unavailable");
                    if ("launch_stub".equals(command)) {
                        if (args.length() != 1 || !args.has("appId") || !(args.get("appId") instanceof String))
                            throw new IllegalArgumentException("An application ID is required");
                        long identity = Binder.clearCallingIdentity();
                        try { result = flatpakStubManager.launch(args.getString("appId")); }
                        finally { Binder.restoreCallingIdentity(identity); }
                    } else if ("install".equals(command)) {
                        /* Flow B: stage, create the stub, then deploy to the
                         * stub UID once PackageManager assigns it. */
                        if (!args.has("ref") || !(args.get("ref") instanceof String))
                            throw new IllegalArgumentException("An application reference is required");
                        result = flatpakStubManager.installAsync(args.getString("ref"), args);
                    } else {
                        if (("add_flathub".equals(command) || "list_remotes".equals(command)) && !args.has("runtimeUid")) {
                            int runtimeUid = runtimesUid(UserHandle.getUserId(Binder.getCallingUid()));
                            if (runtimeUid >= 10000) args.put("runtimeUid", runtimeUid);
                        }
                        result = service.call(command, args.toString());
                    }
                } else {
                    IChannel channel = channelFor(target);
                    if (channel == null) throw new IllegalStateException("channel instance unavailable: " + target);
                    result = channel.call(command, args.toString());
                }
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
            if ("flatpak".equals(target)) {
                if (flatpakSubscriptions.containsKey(key)) return;
                ILinuxd service = linuxdFor();
                if (service == null) throw new IllegalStateException("Flatpak service is unavailable");
                FlatpakSubscription subscription = new FlatpakSubscription(key, topic, listener, service);
                FlatpakSubscription previous = flatpakSubscriptions.putIfAbsent(key, subscription);
                if (previous != null) return;
                try { service.subscribe(topic, subscription.serviceListener); }
                catch (RemoteException e) {
                    flatpakSubscriptions.remove(key, subscription);
                    throw new IllegalStateException("Flatpak subscribe failed", e);
                }
                return;
            }
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
            String key = subscriptionKey(listener.asBinder(), target, topic);
            if ("flatpak".equals(target)) {
                FlatpakSubscription subscription = flatpakSubscriptions.remove(key);
                if (subscription != null) subscription.close();
                return;
            }
            Subscription subscription = subscriptions.remove(key);
            if (subscription != null) subscription.close();
        }
    };

    @Override public IBinder onBind(Intent intent) {
        // Android enforces the service's SYSTEM_BRIDGE permission before bind.
        return binder;
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        applyNavigationProviderSelection();
        applyAbsolutePointerModeIfPresent();
        return START_STICKY;
    }

    @Override public void onCreate() {
        super.onCreate();
        activeService = this;
        flatpakStubManager = new FlatpakStubManager(this);
        flatpakStubManager.start();
        // sleep: poll off the main thread; failed power inspection keeps sleep blocked.
        sleepWakeForwarder = new AndroidWakeStateForwarder(this);
        sleepWakeForwarder.start();
        grantBridgeOverlayAppOps();
        ensureBuiltInShelfDefault();
        navigationBarWindow = new NavigationBarWindow(this);
        applyNavigationProviderSelection();
        geometryHandler = new android.os.Handler(getMainLooper());
        DisplayManager displayManager = getSystemService(DisplayManager.class);
        if (displayManager != null) {
            displayManager.registerDisplayListener(displayListener, new android.os.Handler(getMainLooper()));
            publishDisplayGeometry();
        }
        // Absolute VM/tablet input is translated to relative motion by inputd.
        // Keep InputReader's global cursor transform linear and unit gain, but
        // only while such a device exists (inputd may find it after we start).
        applyAbsolutePointerModeIfPresent();
        for (long delay : new long[] {5000, 15000, 30000}) {
            geometryHandler.postDelayed(this::applyAbsolutePointerModeIfPresent, delay);
        }
        // The bridge is bound explicitly by apps. Shell administration is exposed
        // through the UID-gated ContentProvider below, never ServiceManager.
    }

    @Override public void onDestroy() {
        if (flatpakStubManager != null) flatpakStubManager.close();
        // sleep: stop the poller with the persistent bridge service.
        if (sleepWakeForwarder != null) sleepWakeForwarder.stop();
        if (navigationBarWindow != null) navigationBarWindow.close();
        if (geometryHandler != null) geometryHandler.removeCallbacksAndMessages(null);
        if (activeService == this) activeService = null;
        super.onDestroy();
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

    private boolean absolutePointerModeApplied;

    /** Unit-gain cursor settings only when inputd reports an absolute pointer. */
    private void applyAbsolutePointerModeIfPresent() {
        if (absolutePointerModeApplied) return;
        String count = android.os.SystemProperties.get("vendor.maton.input.absolute", "0");
        if ("0".equals(count) || count.isEmpty()) return;
        try {
            Log.i(TAG, "Absolute pointer settings applied (" + count + " absolute devices): "
                    + setAbsolutePointerMode(UserHandle.USER_CURRENT));
            absolutePointerModeApplied = true;
        } catch (RuntimeException | JSONException e) {
            Log.e(TAG, "Cannot apply absolute pointer settings", e);
        }
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

    private void grantBridgeOverlayAppOps() {
        try {
            AppOpsManager appOps = getSystemService(AppOpsManager.class);
            int uid = android.os.Process.myUid();
            appOps.setMode(AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW, uid, getPackageName(),
                    AppOpsManager.MODE_ALLOWED);
            appOps.setMode(AppOpsManager.OPSTR_SYSTEM_APPLICATION_OVERLAY, uid, getPackageName(),
                    AppOpsManager.MODE_ALLOWED);
            Log.i(TAG, "Enabled app-ops for bridge-owned navigation host window");
        } catch (RuntimeException failure) {
            Log.e(TAG, "Could not enable bridge navigation overlay app-ops", failure);
        }
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
            state.put("userChoiceMade", prefs.getBoolean("userChoiceMade", false));
        } catch (JSONException impossible) { }
        return state;
    }

    private void applyNavigationProviderSelection() {
        if (navigationBarWindow == null) return;
        android.content.SharedPreferences prefs = getSharedPreferences(
                "navigation_bar_provider", MODE_PRIVATE);
        String pkg = prefs.getString("package", NavigationBarWindow.DEFAULT_PROVIDER);
        String cert = prefs.getString("certificate", null);
        String serviceClass = prefs.getString("serviceClass", null);
        boolean enabled = prefs.getBoolean("enabled", false);
        navigationBarWindow.select(pkg, cert, serviceClass, enabled);
    }

    /** Enable the image-bundled Shelf by default only when its pinned app key matches. */
    private void ensureBuiltInShelfDefault() {
        android.content.SharedPreferences prefs = getSharedPreferences(
                "navigation_bar_provider", MODE_PRIVATE);
        if (prefs.getBoolean("userChoiceMade", false)) return;
        String existingPackage = prefs.getString("package", null);
        // Before this default existed, an explicit revoke was stored as enabled=false
        // without the newer userChoiceMade marker. Preserve that revocation.
        if (prefs.contains("enabled") && !prefs.getBoolean("enabled", false)) {
            prefs.edit().putBoolean("userChoiceMade", true).apply();
            return;
        }
        if (existingPackage != null && !NavigationBarWindow.DEFAULT_PROVIDER.equals(existingPackage)) {
            // Preserve legacy explicit selections written before userChoiceMade existed.
            prefs.edit().putBoolean("userChoiceMade", true).apply();
            return;
        }
        String pinned = pinnedCertificateForKey(this, "matonos-shelf");
        String installed = certificateFor(this, NavigationBarWindow.DEFAULT_PROVIDER);
        if (pinned == null || !pinned.equals(installed)) {
            Log.e(TAG, "Built-in Shelf certificate does not match caller_cert_allowlist; default remains disabled");
            return;
        }
        String serviceClass = providerServiceClass(this, NavigationBarWindow.DEFAULT_PROVIDER);
        if (serviceClass == null) {
            Log.e(TAG, "Built-in Shelf has no visible navigation provider service; default remains disabled");
            return;
        }
        prefs.edit().putString("package", NavigationBarWindow.DEFAULT_PROVIDER)
                .putString("certificate", pinned).putString("serviceClass", serviceClass)
                .putBoolean("enabled", true).putBoolean("userChoiceMade", false)
                .putBoolean("builtInDefault", true).apply();
        Log.i(TAG, "Enabled image-bundled Shelf as the certificate-pinned default navigation provider");
    }

    static String pinnedCertificateForKey(Context context, String keyId) {
        if (keyId == null) return null;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                context.getResources().openRawResource(R.raw.caller_cert_allowlist), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] fields = line.trim().split("\\s+");
                if (fields.length == 2 && fields[0].equals(keyId)) return fields[1].toLowerCase(Locale.ROOT);
                // Compatibility with the pre-key-id generated hash-only file.
                if (fields.length == 1 && keyId.equals("matonos-shelf")
                        && fields[0].matches("(?i)[0-9a-f]{64}")) {
                    String installed = certificateFor(context, NavigationBarWindow.DEFAULT_PROVIDER);
                    if (fields[0].equalsIgnoreCase(installed)) return fields[0].toLowerCase(Locale.ROOT);
                }
            }
        } catch (Exception ignored) { }
        return null;
    }

    static String providerServiceClass(Context context, String packageName) {
        if (packageName == null || packageName.isEmpty()) return null;
        android.content.Intent intent = new android.content.Intent(NavigationBarWindow.PROVIDER_ACTION)
                .setPackage(packageName);
        java.util.List<android.content.pm.ResolveInfo> candidates =
                context.getPackageManager().queryIntentServices(intent, 0);
        if (candidates.size() != 1 || candidates.get(0).serviceInfo == null
                || !candidates.get(0).serviceInfo.exported) return null;
        return candidates.get(0).serviceInfo.name;
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
        String pkg = BridgeCallerIdentity.solePackage(packages);
        if (pkg == null) {
            deny(operation, uid, "caller UID does not map to exactly one installed package");
        }
        if (selected.equals(pkg)) {
            try {
                PackageInfo info = getPackageManager().getPackageInfo(pkg,
                        PackageManager.GET_SIGNING_CERTIFICATES);
                String current = certificateFor(this, pkg);
                String stillSolePackage = BridgeCallerIdentity.solePackage(
                        getPackageManager().getPackagesForUid(uid));
                if (info.applicationInfo.uid == uid && pkg.equals(stillSolePackage)
                        && pinnedCertificate.equals(current)) {
                    Log.i(TAG, "Authorized provider action=" + operation + " target=" + target
                            + " package=" + pkg + " uid=" + uid);
                    return pkg;
                }
            } catch (PackageManager.NameNotFoundException ignored) {
                // Package disappeared between UID attribution and certificate check.
            }
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

    static void selectNavigationProvider(Context context, String packageName, boolean enabled) {
        android.content.SharedPreferences prefs = context.getSharedPreferences(
                "navigation_bar_provider", MODE_PRIVATE);
        if (packageName == null || packageName.isEmpty()) {
            if (enabled) throw new IllegalArgumentException("provider package is required");
            prefs.edit().putBoolean("enabled", false).putBoolean("userChoiceMade", true)
                    .putBoolean("builtInDefault", false).apply();
            Log.i(TAG, "Navigation provider explicitly revoked by user");
        } else {
            String certificate = certificateFor(context, packageName);
            if ("unavailable".equals(certificate) || "package unavailable".equals(certificate))
                throw new IllegalArgumentException("selected provider is not installed or has no signing certificate");
            String serviceClass = providerServiceClass(context, packageName);
            if (serviceClass == null)
                throw new IllegalArgumentException("selected package must export exactly one MatonOS navigation provider service");
            if (NavigationBarWindow.DEFAULT_PROVIDER.equals(packageName)
                    && !certificate.equals(pinnedCertificateForKey(context, "matonos-shelf")))
                throw new IllegalArgumentException("built-in Shelf certificate does not match the pinned image key");
            prefs.edit().putString("package", packageName).putString("certificate", certificate)
                    .putString("serviceClass", serviceClass).putBoolean("enabled", enabled)
                    .putBoolean("userChoiceMade", true)
                    .putBoolean("builtInDefault", false).apply();
            Log.i(TAG, (enabled ? "User selected" : "User preset") + " navigation provider=" + packageName
                    + " certificate=" + certificate + " enabled=" + enabled);
        }
        SystemBridgeService service = activeService;
        if (service != null) service.applyNavigationProviderSelection();
        else context.startService(new Intent(context, SystemBridgeService.class));
    }

    private void enforcePermission(int uid, String operation) {
        if (checkPermission(PERMISSION, -1, uid) != PackageManager.PERMISSION_GRANTED)
            deny(operation, uid, "missing permission");
    }

    private String enforceAuthorizedCaller(String target, String operation) {
        int uid = Binder.getCallingUid();
        enforcePermission(uid, operation);
        enforceNotBanned(uid, operation);
        String[] packages = getPackageManager().getPackagesForUid(uid);
        if (BridgeCallerIdentity.solePackage(packages) == null) {
            deny(operation, uid, "caller UID does not map to exactly one installed package");
        }
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
        String packageName = BridgeCallerIdentity.solePackage(packages);
        if (packageName == null) return null;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.applicationInfo.uid != uid || info.signingInfo == null) return null;
            android.content.pm.Signature[] signers = info.signingInfo.getApkContentsSigners();
            if (signers == null || signers.length != 1) return null;
            String cert = sha256(signers[0].toByteArray());
            if (isBanned(this, packageName, cert)) {
                Log.w(TAG, "Denied banned package=" + packageName + " target=" + target);
                return null;
            }
            boolean builtin = packagesForTarget.contains(target + " " + packageName)
                    && certs.contains(cert);
            boolean trusted = trustedTargets(packageName, cert).contains(target);
            return BridgeCallerIdentity.authorizedPackage(
                    getPackageManager().getPackagesForUid(uid),
                    builtin || trusted ? packageName : null);
        } catch (PackageManager.NameNotFoundException ignored) {
            // Package disappeared between UID attribution and certificate check.
            return null;
        }
    }

    private Set<String> trustedTargets(String packageName, String cert) {
        String json = getSharedPreferences("trusted_developer_apps", MODE_PRIVATE)
                .getString(packageName, null);
        if (json == null) return java.util.Collections.emptySet();
        try {
            JSONObject entry = new JSONObject(json);
            if (!BridgeCallerIdentity.certificateMatches(cert,
                    entry.getString("certSha256"))) return java.util.Collections.emptySet();
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
                "audio", "camera", "install", "addons", "flatpak", "flatpak_launch", "status", "nav.back", "nav.home", "nav.recents"));
    }

    private static void grantTrust(android.content.Context context, String packageName, Set<String> targets) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.signingInfo == null || info.signingInfo.getApkContentsSigners().length != 1)
                throw new IllegalArgumentException("package must have one current signing certificate");
            String[] packages = context.getPackageManager().getPackagesForUid(info.applicationInfo.uid);
            if (BridgeCallerIdentity.solePackage(packages) == null)
                throw new IllegalArgumentException("packages sharing a UID cannot receive package-scoped bridge trust");
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
            if (readCertificateAllowlist(context).contains(cert)) { builtin = true; break; }
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
                } else if ("installer_test_call".equals(method)) {
                    // Debug-only end-to-end harness. Root must explicitly arm it;
                    // production app calls still use the normal cert allowlist.
                    if (uid != 0 || !Build.IS_DEBUGGABLE ||
                            !"1".equals(android.os.SystemProperties.get("persist.vendor.maton.installer_test")) ||
                            !"1".equals(android.os.SystemProperties.get("ro.boot.matonos.live")))
                        throw new SecurityException("installer test channel requires an armed live debug image and root");
                    if (!Set.of("get_status", "list_drives", "execute_operation", "cancel_operation").contains(arg))
                        throw new IllegalArgumentException("unsupported installer test command");
                    String encoded = extras == null ? null : extras.getString("payload_b64");
                    if (encoded == null || encoded.length() > 90000)
                        throw new IllegalArgumentException("installer request payload is missing or too large");
                    String request = new String(Base64.getDecoder().decode(encoded), StandardCharsets.UTF_8);
                    SystemBridgeService bridge = activeService;
                    IChannel channel = bridge == null ? null : bridge.channelFor("install");
                    if (channel == null) throw new IllegalStateException("install service is unavailable");
                    try {
                        result.putString("result", channel.call(arg, request));
                    } catch (RemoteException e) {
                        throw new IllegalStateException("install service call failed", e);
                    }
                } else if ("addons_test_call".equals(method)) {
                    // addons: debug-only harness, same arming rules as installer_test_call.
                    if (uid != 0 || !Build.IS_DEBUGGABLE ||
                            !"1".equals(android.os.SystemProperties.get("persist.vendor.maton.addons_test")) ||
                            !"1".equals(android.os.SystemProperties.get("ro.boot.matonos.live")))
                        throw new SecurityException("addons test channel requires an armed live debug image and root");
                    if (!Set.of("status", "available", "begin_package", "package_chunk", "commit_package",
                            "begin_image", "commit_image", "uninstall").contains(arg))
                        throw new IllegalArgumentException("unsupported addons test command");
                    String encoded = extras == null ? null : extras.getString("payload_b64");
                    if (encoded == null || encoded.length() > 90000)
                        throw new IllegalArgumentException("addons request payload is missing or too large");
                    String request = new String(Base64.getDecoder().decode(encoded), StandardCharsets.UTF_8);
                    SystemBridgeService bridge = activeService;
                    IChannel channel = bridge == null ? null : bridge.channelFor("addons");
                    if (channel == null) throw new IllegalStateException("addons service is unavailable");
                    try {
                        result.putString("result", channel.call(arg, request));
                    } catch (RemoteException e) {
                        throw new IllegalStateException("addons service call failed", e);
                    }
                } else if ("flatpak_test_call".equals(method)) {
                    // Root-only live debug harness. Production callers use the normal flatpak ACL.
                    if (uid != 0 || !Build.IS_DEBUGGABLE ||
                            !"1".equals(android.os.SystemProperties.get("persist.vendor.maton.flatpak_test")) ||
                            !"1".equals(android.os.SystemProperties.get("ro.boot.matonos.live")))
                        throw new SecurityException("flatpak test channel requires an armed live debug image and root");
                    if (!Set.of("stage", "list_installed", "list_remotes", "add_flathub", "install",
                            "uninstall", "kill").contains(arg))
                        throw new IllegalArgumentException("unsupported flatpak test command");
                    String encoded = extras == null ? null : extras.getString("payload_b64");
                    if (encoded == null || encoded.length() > 90000)
                        throw new IllegalArgumentException("Flatpak request payload is missing or too large");
                    String request = new String(Base64.getDecoder().decode(encoded), StandardCharsets.UTF_8);
                    SystemBridgeService bridge = activeService;
                    ILinuxd linuxd = bridge == null ? null : bridge.linuxdFor();
                    if (linuxd == null) throw new IllegalStateException("Flatpak service is unavailable");
                    try {
                        result.putString("result", linuxd.call(arg, request));
                    } catch (RemoteException e) {
                        throw new IllegalStateException("Flatpak service call failed", e);
                    }
                } else throw new IllegalArgumentException("unknown bridge shell method");
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
            if (allowedCerts == null) allowedCerts = readCertificateAllowlist(this);
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

    private static Set<String> readCertificateAllowlist(android.content.Context context) {
        Set<String> result = new HashSet<>();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                context.getResources().openRawResource(R.raw.caller_cert_allowlist), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] fields = line.split("\\s+");
                String cert = fields.length == 1 ? fields[0] : fields[fields.length - 1];
                if (cert.matches("(?i)[0-9a-f]{64}")) result.add(cert.toLowerCase(Locale.ROOT));
            }
        } catch (Exception e) {
            Log.e(TAG, "Cannot load bridge certificate allowlist", e);
        }
        return result;
    }

    private IChannel channelFor(String target) {
        // checkService is deliberately nonblocking: optional hardware instances may be absent.
        IBinder service = ServiceManager.checkService("vendor.matonos.channel.IChannel/" + target);
        return IChannel.Stub.asInterface(service);
    }

    private ILinuxd linuxdFor() {
        IBinder service = ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default");
        return ILinuxd.Stub.asInterface(service);
    }

    /** UID of the preinstalled runtime app that owns the shared --system install. */
    private int runtimesUid(int userId) {
        try {
            return getPackageManager().getApplicationInfoAsUser(RUNTIMES_PACKAGE, 0, userId).uid;
        } catch (Exception error) {
            Log.w(TAG, "Runtime app UID unavailable for user " + userId);
            return -1;
        }
    }

    private boolean channelAvailable(String target) {
        return "flatpak".equals(target) ? linuxdFor() != null : channelFor(target) != null;
    }

    private final class FlatpakSubscription {
        final String key;
        final String topic;
        final IMatonosListener appListener;
        final ILinuxd service;
        final ILinuxdListener serviceListener;
        FlatpakSubscription(String key, String topic, IMatonosListener appListener, ILinuxd service) {
            this.key = key;
            this.topic = topic;
            this.appListener = appListener;
            this.service = service;
            this.serviceListener = new ILinuxdListener.Stub() {
                @Override public void onEvent(String eventTopic, String json) {
                    if (!topic.equals(eventTopic) || json == null || json.length() > 64 * 1024) return;
                    try { appListener.onEvent("flatpak", eventTopic, json); }
                    catch (RemoteException e) {
                        Log.w(TAG, "Flatpak listener died for " + topic, e);
                        close();
                    }
                }
            };
        }
        void close() {
            flatpakSubscriptions.remove(key, this);
            try { service.unsubscribe(topic, serviceListener); }
            catch (RemoteException e) { Log.w(TAG, "Flatpak unsubscribe failed", e); }
        }
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
