package org.matonos.client;

import android.Manifest;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.Bundle;
import android.provider.Settings;
import android.view.WindowManager;

import org.json.JSONArray;
import org.json.JSONObject;
import org.matonos.systembridge.IMatonosListener;
import org.matonos.systembridge.ISystemBridge;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;

/** Shared client for the MatonOS system bridge and its stable JSON channel. */
public final class MatonosClient implements AutoCloseable {
    private static final Set<MatonosClient> INSTANCES = ConcurrentHashMap.newKeySet();
    public interface EventListener { void onEvent(String target, String topic, String jsonEvent); }
    public interface AvailabilityListener { void onAvailabilityChanged(boolean available, String reason); }
    public interface CheckCallback { void onComplete(CheckResult result); }

    public static final int REQUIRED_BRIDGE_API = 5;
    private static final String BRIDGE_PACKAGE = "org.matonos.systembridge";
    private static final String BRIDGE_ACTION = "org.matonos.systembridge.BIND";
    private static final String TRUST_RESULT_PREFIX = "org.matonos.client.TRUST_RESULT.";
    private static final long BIND_TIMEOUT_MS = 6000;

    public static final class Result<T> {
        public final boolean available;
        public final T value;
        public final String reason;
        private Result(boolean available, T value, String reason) {
            this.available = available; this.value = value; this.reason = reason;
        }
        public static <T> Result<T> value(T value) { return new Result<>(true, value, null); }
        public static <T> Result<T> unavailable(String reason) { return new Result<>(false, null, reason); }
    }

    public static final class CheckResult {
        public final boolean available;
        public final String reason;
        public final int bridgeApiVersion;
        private CheckResult(boolean available, String reason, int version) {
            this.available = available; this.reason = reason; this.bridgeApiVersion = version;
        }
    }

    public boolean isTrustRequestEnabled() { return MatonOS.trustRequestsEnabled(context); }

    private static final class PendingCheck {
        final Set<String> accessTargets;
        final Set<String> channelTargets;
        final CheckCallback callback;
        PendingCheck(Set<String> access, Set<String> channels, CheckCallback callback) {
            accessTargets = access; channelTargets = channels; this.callback = callback;
        }
    }

    private final Context context;
    private final BridgeMode mode;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<PendingCheck> pending = new CopyOnWriteArrayList<>();
    private final List<AvailabilityListener> availabilityListeners = new CopyOnWriteArrayList<>();
    private final Map<String, IMatonosListener> callbacks = new ConcurrentHashMap<>();
    private volatile ISystemBridge bridge;
    private volatile boolean bound;
    private volatile boolean closed;
    private volatile int bridgeApiVersion;
    private volatile String reason = "BRIDGE_CONNECTING";
    private volatile Set<String> allowedTargets = Collections.emptySet();
    private volatile Set<String> availableChannels = Collections.emptySet();
    private final Runnable bindTimeout = () -> {
        if (bridge == null && !closed) {
            setReason("BRIDGE_UNAVAILABLE");
            finishPending(new CheckResult(false, reason, bridgeApiVersion));
        }
    };

    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            bridge = ISystemBridge.Stub.asInterface(service);
            main.removeCallbacks(bindTimeout);
            new Thread(MatonosClient.this::refreshBridgeState, "matonos-bridge-check").start();
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            bridge = null;
            setReason("BRIDGE_DISCONNECTED");
        }
        @Override public void onBindingDied(ComponentName name) {
            bridge = null; setReason("BRIDGE_UNAVAILABLE");
            if (bound) { context.unbindService(this); bound = false; }
        }
        @Override public void onNullBinding(ComponentName name) {
            bridge = null; setReason("BRIDGE_UNAVAILABLE");
        }
    };

    public MatonosClient(Context context) { this(context, MatonOS.modeFromManifest(context)); }

    public MatonosClient(Context context, BridgeMode mode) {
        this.context = context.getApplicationContext();
        this.mode = mode == null ? BridgeMode.REQUIRED : mode;
        INSTANCES.add(this);
        connect();
    }

    public BridgeMode getMode() { return mode; }
    public boolean isAvailable() { return reason == null; }
    public String getReason() { return reason; }
    public int getBridgeApiVersion() { return bridgeApiVersion; }
    public boolean isConnected() { return bridge != null; }
    public boolean isTargetAllowed(String target) { return allowedTargets.contains(target); }
    public boolean isChannelAvailable(String target) { return availableChannels.contains(target); }
    public ISystemBridge getBridgeInterface() { return bridge; }

    public void addAvailabilityListener(AvailabilityListener listener) {
        if (listener == null) return;
        availabilityListeners.add(listener);
        listener.onAvailabilityChanged(isAvailable(), reason);
    }
    public void removeAvailabilityListener(AvailabilityListener listener) { availabilityListeners.remove(listener); }

    public void checkStartup(String[] accessTargets, String[] channelTargets, CheckCallback callback) {
        if (callback == null) return;
        PendingCheck check = new PendingCheck(setOf(accessTargets), setOf(channelTargets), callback);
        pending.add(check);
        if (bridge != null && reason == null) evaluatePending();
        else connect();
    }

    public void refresh() {
        if (bridge != null) new Thread(this::refreshBridgeState, "matonos-bridge-refresh").start();
        else connect();
    }

    static void refreshAll() { for (MatonosClient client : INSTANCES) client.refresh(); }

    private void connect() {
        if (closed || bridge != null || bound) return;
        if (context.checkSelfPermission(MatonOS.PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            setReason("BRIDGE_PERMISSION_REQUIRED");
            finishPending(new CheckResult(false, reason, bridgeApiVersion));
            return;
        }
        try {
            ServiceInfo info = context.getPackageManager().getServiceInfo(
                    new ComponentName(BRIDGE_PACKAGE, BRIDGE_PACKAGE + ".SystemBridgeService"),
                    PackageManager.MATCH_DISABLED_COMPONENTS);
            if (info == null || !info.enabled) {
                setReason("BRIDGE_NOT_INSTALLED");
                finishPending(new CheckResult(false, reason, bridgeApiVersion));
                return;
            }
        } catch (PackageManager.NameNotFoundException e) {
            setReason("BRIDGE_NOT_INSTALLED");
            finishPending(new CheckResult(false, reason, bridgeApiVersion));
            return;
        }
        Intent intent = new Intent(BRIDGE_ACTION).setComponent(
                new ComponentName(BRIDGE_PACKAGE, BRIDGE_PACKAGE + ".SystemBridgeService"));
        bound = context.bindService(intent, connection, Context.BIND_AUTO_CREATE);
        if (!bound) {
            setReason("BRIDGE_UNAVAILABLE");
            finishPending(new CheckResult(false, reason, bridgeApiVersion));
        } else {
            setReason("BRIDGE_CONNECTING");
            main.postDelayed(bindTimeout, BIND_TIMEOUT_MS);
        }
    }

    private void refreshBridgeState() {
        ISystemBridge current = bridge;
        if (current == null || closed) return;
        try {
            int version = current.getBridgeApiVersion();
            String status = current.getBridgeStatus();
            JSONObject json = new JSONObject(status);
            if (json.optBoolean("banned", false)) {
                bridgeApiVersion = version;
                allowedTargets = Collections.emptySet();
                availableChannels = Collections.emptySet();
                setReason("BRIDGE_NOT_INSTALLED");
                finishPending(new CheckResult(false, reason, bridgeApiVersion));
                return;
            }
            Set<String> allowed = jsonArray(json.optJSONArray("allowedTargets"));
            Set<String> channels = jsonArray(json.optJSONArray("availableChannels"));
            bridgeApiVersion = version;
            allowedTargets = allowed;
            availableChannels = channels;
            if (version < REQUIRED_BRIDGE_API) setReason("BRIDGE_UPDATE_REQUIRED");
            else if (allowed.isEmpty()) setReason("BRIDGE_APP_NOT_TRUSTED");
            else setReason(null);
            evaluatePending();
        } catch (Exception e) {
            bridgeApiVersion = 0;
            allowedTargets = Collections.emptySet();
            availableChannels = Collections.emptySet();
            setReason(e instanceof SecurityException
                    && "BRIDGE_NOT_INSTALLED".equals(e.getMessage())
                    ? "BRIDGE_NOT_INSTALLED" : "BRIDGE_UNAVAILABLE");
            finishPending(new CheckResult(false, reason, bridgeApiVersion));
        }
    }

    private void evaluatePending() {
        if (bridge == null || reason != null) return;
        for (PendingCheck check : new ArrayList<>(pending)) {
            String failure = null;
            for (String target : check.accessTargets) {
                if (!allowedTargets.contains(target)) { failure = "BRIDGE_APP_NOT_TRUSTED"; break; }
            }
            if (failure == null) for (String target : check.channelTargets) {
                if (!availableChannels.contains(target)) { failure = "CHANNEL_UNAVAILABLE:" + target; break; }
            }
            pending.remove(check);
            check.callback.onComplete(new CheckResult(failure == null, failure, bridgeApiVersion));
        }
    }

    private void finishPending(CheckResult result) {
        if (result.reason != null && "BRIDGE_CONNECTING".equals(result.reason)) return;
        for (PendingCheck check : new ArrayList<>(pending)) {
            pending.remove(check); check.callback.onComplete(result);
        }
    }

    private void setReason(String value) {
        boolean oldAvailable = reason == null;
        String oldReason = reason;
        reason = value;
        boolean newAvailable = reason == null;
        if (oldAvailable != newAvailable || !java.util.Objects.equals(oldReason, value)) {
            for (AvailabilityListener listener : availabilityListeners)
                listener.onAvailabilityChanged(newAvailable, value);
        }
    }

    public Result<String> call(String target, String command, String jsonArgs) {
        if (!isAvailable()) return Result.unavailable(reason);
        if (!isTargetAllowed(target)) return Result.unavailable("BRIDGE_APP_NOT_TRUSTED:" + target);
        ISystemBridge current = bridge;
        if (current == null) return Result.unavailable("BRIDGE_DISCONNECTED");
        try { return Result.value(current.call(target, command, jsonArgs)); }
        catch (Exception e) { return Result.unavailable(reasonFor(e)); }
    }

    public Result<Boolean> injectBackKey() {
        if (!isAvailable()) return Result.unavailable(reason);
        ISystemBridge current = bridge;
        if (current == null) return Result.unavailable("BRIDGE_DISCONNECTED");
        try { return Result.value(current.injectBackKey()); }
        catch (Exception e) { return Result.unavailable(reasonFor(e)); }
    }

    public Result<String> getRecentTasks(int maxTasks) {
        return invoke("launcher", b -> b.getRecentTasks(maxTasks));
    }

    public Result<Boolean> removeRecentTask(int taskId) {
        return invoke("launcher", b -> b.removeRecentTask(taskId));
    }

    public Result<Boolean> moveTaskToFront(int taskId) {
        return invoke("launcher", b -> b.moveTaskToFront(taskId));
    }

    public Result<Boolean> setTaskFullscreen(int taskId) {
        return invoke("launcher", b -> b.setTaskFullscreen(taskId));
    }

    public Result<Boolean> ensureShellOverlayAccess() {
        return invoke("launcher", ISystemBridge::ensureShellOverlayAccess);
    }

    public Result<Boolean> navigateBack(boolean longPress) {
        return invoke("nav.back", b -> b.navigateBack(longPress));
    }

    public Result<Boolean> navigateHome() {
        return invoke("nav.home", ISystemBridge::navigateHome);
    }

    public Result<Boolean> navigateRecents() {
        return invoke("nav.recents", ISystemBridge::navigateRecents);
    }

    public Result<byte[]> getRecentTaskThumbnail(int taskId) {
        return invoke("launcher", b -> b.getRecentTaskThumbnail(taskId));
    }

    public Result<Bundle> prepareShellOverlay(WindowManager.LayoutParams params) {
        ISystemBridge current = bridge;
        if (!isAvailable() || current == null) return Result.unavailable(reason == null ? "BRIDGE_DISCONNECTED" : reason);
        Bundle request = new Bundle();
        request.putParcelable("windowParams", params);
        try { return Result.value(current.prepareShellOverlay(request)); }
        catch (Exception e) { return Result.unavailable(reasonFor(e)); }
    }

    private interface BridgeOperation<T> { T call(ISystemBridge bridge) throws Exception; }

    private <T> Result<T> invoke(String target, BridgeOperation<T> operation) {
        if (!isAvailable()) return Result.unavailable(reason == null ? "BRIDGE_UNAVAILABLE" : reason);
        if (!isTargetAllowed(target)) return Result.unavailable("BRIDGE_APP_NOT_TRUSTED:" + target);
        ISystemBridge current = bridge;
        if (current == null) return Result.unavailable("BRIDGE_DISCONNECTED");
        try { return Result.value(operation.call(current)); }
        catch (Exception e) { return Result.unavailable(reasonFor(e)); }
    }

    /** Disable pointer acceleration and select the slowest unaccelerated speed for absolute input. */
    public Result<String> setAbsolutePointerMode() {
        return call("input", "set_absolute_pointer_mode", "{}");
    }

    public Result<Void> subscribe(String target, String topic, EventListener listener) {
        if (!isAvailable()) return Result.unavailable(reason);
        if (!isTargetAllowed(target)) return Result.unavailable("BRIDGE_APP_NOT_TRUSTED:" + target);
        ISystemBridge current = bridge;
        if (current == null) return Result.unavailable("BRIDGE_DISCONNECTED");
        String key = target + ':' + topic + ':' + System.identityHashCode(listener);
        IMatonosListener callback = new IMatonosListener.Stub() {
            @Override public void onEvent(String eventTarget, String eventTopic, String jsonEvent) {
                if (listener != null) listener.onEvent(eventTarget, eventTopic, jsonEvent);
            }
        };
        callbacks.put(key, callback);
        try { current.subscribe(target, topic, callback); return Result.value(null); }
        catch (Exception e) { callbacks.remove(key); return Result.unavailable(reasonFor(e)); }
    }

    public Result<Void> unsubscribe(String target, String topic, EventListener listener) {
        String key = target + ':' + topic + ':' + System.identityHashCode(listener);
        IMatonosListener callback = callbacks.remove(key);
        if (callback == null) return Result.value(null);
        ISystemBridge current = bridge;
        if (current == null) return Result.unavailable("BRIDGE_DISCONNECTED");
        try { current.unsubscribe(target, topic, callback); return Result.value(null); }
        catch (Exception e) { return Result.unavailable(reasonFor(e)); }
    }

    private String reasonFor(Exception error) {
        if (error instanceof SecurityException && "BRIDGE_NOT_INSTALLED".equals(error.getMessage()))
            return "BRIDGE_NOT_INSTALLED";
        if (error instanceof SecurityException) return "BRIDGE_APP_NOT_TRUSTED";
        return "BRIDGE_UNAVAILABLE";
    }

    private static Set<String> setOf(String[] values) {
        if (values == null || values.length == 0) return Collections.emptySet();
        Set<String> result = new HashSet<>(); Collections.addAll(result, values); return result;
    }
    private static Set<String> jsonArray(JSONArray array) {
        if (array == null) return Collections.emptySet();
        Set<String> result = new HashSet<>();
        for (int i = 0; i < array.length(); i++) {
            String value = array.optString(i, null); if (value != null) result.add(value);
        }
        return result;
    }

    @Override public void close() {
        closed = true;
        INSTANCES.remove(this);
        main.removeCallbacks(bindTimeout);
        ISystemBridge current = bridge;
        if (current != null) for (Map.Entry<String, IMatonosListener> entry : callbacks.entrySet()) {
            String[] parts = entry.getKey().split(":", 3);
            try { current.unsubscribe(parts[0], parts[1], entry.getValue()); }
            catch (RemoteException ignored) { }
        }
        callbacks.clear(); pending.clear(); availabilityListeners.clear();
        bridge = null;
        if (bound) { context.unbindService(connection); bound = false; }
    }
}
