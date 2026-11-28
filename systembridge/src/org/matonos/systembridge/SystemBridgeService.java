package org.matonos.systembridge;

import android.app.Service;
import android.media.AudioDeviceAttributes;
import android.media.AudioManager;
import android.media.AudioSystem;
import android.media.AudioAttributes;
import android.media.audiopolicy.AudioProductStrategy;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Build;
import android.os.Handler;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.security.MessageDigest;
import java.util.List;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Locale;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import java.util.Base64;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicBoolean;

import vendor.matonos.channel.IChannel;
import vendor.matonos.channel.IChannelListener;
import org.matonos.client.AudioClient;
import org.matonos.client.BridgeMode;
import org.matonos.client.MatonosClient;

public final class SystemBridgeService extends Service {
    private static final String TAG = "MatonSystemBridge";
    private static final String PERMISSION = "org.matonos.permission.SYSTEM_BRIDGE";
    private static volatile SystemBridgeService activeService;
    private volatile Set<String> allowedCerts;
    private volatile Set<String> targetCallers;
    private final ConcurrentMap<String, Subscription> subscriptions = new ConcurrentHashMap<>();
    // sleep: forward Android's current suspend blockers to the sleep daemon.
    private AndroidWakeStateForwarder sleepWakeForwarder;
    // display: framework display APIs, served in-process rather than by a vendor daemon.
    private volatile DisplayChannel displayChannel;
    private volatile TrustChannel trustChannel;
    private WifiCardSwitcher wifiCardSwitcher;
    private BluetoothControllerSwitcher bluetoothControllerSwitcher;
    private final HardwareChannel hardwareChannel = new HardwareChannel();
    private MatonosClient internalAudioBridge;
    private AudioClient internalAudioClient;
    private volatile boolean audioSubscriptionReady;
    private boolean audioSubscriptionCheckPending;
    private final AudioClient.DeviceListener internalAudioDeviceListener =
            this::handleAudioDeviceEvent;
    private final LinkedHashMap<String, AudioDeviceAttributes> connectedAudioOutputs =
            new LinkedHashMap<>();
    private final LinkedHashMap<String, AudioDeviceAttributes> connectedAudioInputs =
            new LinkedHashMap<>();
    private final Handler audioChannelHandler = new Handler(Looper.getMainLooper());
    private final AtomicBoolean audioRebindActive = new AtomicBoolean();
    private final Runnable audioChannelRebinder = new Runnable() {
        @Override public void run() {
            if (audioRebindActive.compareAndSet(false, true)) {
                new Thread(() -> {
                    try { rebindAudioSubscriptions(); }
                    finally { audioRebindActive.set(false); }
                }, "matonos-audio-channel-rebind").start();
            }
            audioChannelHandler.postDelayed(this, 3000);
        }
    };
    private final ISystemBridge.Stub binder = new ISystemBridge.Stub() {
        @Override public int getBridgeApiVersion() {
            enforceNotBanned(Binder.getCallingUid(), "getBridgeApiVersion");
            return 9;
        }
        @Override public String getBridgeApiHash() {
            enforceNotBanned(Binder.getCallingUid(), "getBridgeApiHash");
            return "227f3f7e15ec7c76afaf631fe540a76218408dc6662fd498c1ca9a620880cd6a";
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
                if ("bluetooth".equals(target) && "select_device".equals(command)) {
                    BluetoothControllerSwitcher switcher = bluetoothControllerSwitcher;
                    if (switcher == null) throw new IllegalStateException("Bluetooth controller switcher unavailable");
                    return switcher.switchTo(args.optString("device", ""), args.optBoolean("persist", true));
                }
                if ("wifi".equals(target) && "select_device".equals(command)) {
                    WifiCardSwitcher switcher = wifiCardSwitcher;
                    if (switcher == null) throw new IllegalStateException("Wi-Fi card switcher unavailable");
                    return switcher.switchTo(args.optString("device", ""), args.optBoolean("persist", true));
                }
                if ("camera".equals(target) && "set_role".equals(command)) {
                    String role = args.optString("role", "");
                    String id = args.optString("id", "");
                    if (!("front".equals(role) || "back".equals(role)))
                        throw new IllegalArgumentException("camera role must be front or back");
                    if (!id.isEmpty() && !id.matches("[0-9]+"))
                        throw new IllegalArgumentException("camera id must contain digits or be empty");
                    // One camera holds at most one role: taking a role clears it from the other.
                    String other = "front".equals(role) ? "back" : "front";
                    if (!id.isEmpty() && id.equals(SystemProperties.get("persist.matonos.camera." + other, "")))
                        SystemProperties.set("persist.matonos.camera." + other, "");
                    SystemProperties.set("persist.matonos.camera." + role, id);
                    JSONObject result = new JSONObject();
                    result.put("front", SystemProperties.get("persist.matonos.camera.front", ""));
                    result.put("back", SystemProperties.get("persist.matonos.camera.back", ""));
                    return result.toString();
                }
                if ("camera".equals(target) && "get_roles".equals(command)) {
                    JSONObject result = new JSONObject();
                    result.put("front", SystemProperties.get("persist.matonos.camera.front", ""));
                    result.put("back", SystemProperties.get("persist.matonos.camera.back", ""));
                    return result.toString();
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

        @Override public String stageUpdate(String[] names, ParcelFileDescriptor[] files) {
            enforceAuthorizedCaller("install", "stageUpdate");
            if (names == null || files == null || names.length != files.length)
                throw new IllegalArgumentException("OTA payload names and descriptors must have equal lengths");
            try {
                IChannel channel = channelFor("install");
                if (channel == null) throw new IllegalStateException("install service is unavailable");
                String result = channel.stageUpdate(names, files);
                if (result == null || result.length() > 64 * 1024)
                    throw new IllegalStateException("invalid OTA stage result");
                return result;
            } catch (Exception e) {
                Log.e(TAG, "OTA stage failed: target=install files=" + (names == null ? 0 : names.length), e);
                throw new IllegalStateException("OTA stage failed", e);
            }
        }

        @Override public String finalizeUpdate() {
            enforceAuthorizedCaller("install", "finalizeUpdate");
            try {
                IChannel channel = channelFor("install");
                if (channel == null) throw new IllegalStateException("install service is unavailable");
                return channel.finalizeUpdate();
            } catch (Exception e) {
                Log.e(TAG, "OTA finalize failed: target=install", e);
                throw new IllegalStateException("OTA finalize failed", e);
            }
        }

        @Override public String getUpdateStatus() {
            enforceAuthorizedCaller("install", "getUpdateStatus");
            try {
                IChannel channel = channelFor("install");
                if (channel == null) throw new IllegalStateException("install service is unavailable");
                return channel.getUpdateStatus();
            } catch (Exception e) {
                Log.e(TAG, "OTA status failed: target=install", e);
                throw new IllegalStateException("OTA status failed", e);
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
            try { channel.subscribe(topic, subscription.channelListener); }
            catch (Exception e) {
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

    private void startAudioDeviceForwarding() {
        internalAudioBridge = new MatonosClient(this, BridgeMode.OPTIONAL);
        internalAudioClient = new AudioClient(internalAudioBridge);
        tryStartAudioSubscription();
    }

    private void tryStartAudioSubscription() {
        synchronized (this) {
            if (audioSubscriptionReady || audioSubscriptionCheckPending || internalAudioBridge == null)
                return;
            audioSubscriptionCheckPending = true;
        }
        internalAudioBridge.checkStartup(new String[] {"audio"}, new String[] {"audio"}, result -> {
            synchronized (SystemBridgeService.this) {
                audioSubscriptionCheckPending = false;
            }
            if (!result.available) {
                Log.e(TAG, "Audio channel unavailable during startup: " + result.reason);
                return;
            }
            MatonosClient.Result<Void> subscription =
                    internalAudioClient.subscribe(internalAudioDeviceListener);
            if (!subscription.available) {
                Log.e(TAG, "Cannot subscribe to audio/devices: " + subscription.reason);
                return;
            }
            audioSubscriptionReady = true;
            reconcileAudioSnapshot();
        });
    }

    private void reconcileAudioSnapshot() {
        AudioClient client = internalAudioClient;
        if (client == null) return;
        MatonosClient.Result<String> result = client.devices();
        if (!result.available) {
            Log.e(TAG, "Audio device snapshot failed: " + result.reason);
            return;
        }
        try {
            JSONObject snapshot = new JSONObject(result.value);
            snapshot.put("event", "snapshot");
            handleAudioDeviceEvent(snapshot.toString());
        } catch (JSONException e) {
            Log.e(TAG, "Invalid audio device snapshot", e);
        }
    }

    private synchronized void handleAudioDeviceEvent(String json) {
        try {
            JSONObject event = new JSONObject(json);
            String eventType = event.optString("event", "");
            if ("snapshot".equals(eventType)) {
                JSONArray devices = event.optJSONArray("devices");
                if (devices == null) throw new JSONException("snapshot is missing devices");
                reconcileAudioDevices(devices);
                return;
            }
            JSONObject device = event.optJSONObject("device");
            if (device == null) throw new JSONException("audio event is missing device");
            AudioDeviceAttributes attributes = audioDeviceAttributes(device);
            if (attributes == null) return;
            boolean connected = "add".equals(eventType);
            if (!connected && !"remove".equals(eventType)) {
                Log.e(TAG, "Unknown audio device event=" + eventType);
                return;
            }
            changeAudioConnection(attributes, connected);
        } catch (JSONException | IllegalArgumentException e) {
            Log.e(TAG, "Invalid audio device event: " + json, e);
        }
    }

    private void reconcileAudioDevices(JSONArray devices) throws JSONException {
        LinkedHashMap<String, AudioDeviceAttributes> wantedOutputs = new LinkedHashMap<>();
        LinkedHashMap<String, AudioDeviceAttributes> wantedInputs = new LinkedHashMap<>();
        for (int i = 0; i < devices.length(); i++) {
            JSONObject device = devices.optJSONObject(i);
            if (device == null) throw new JSONException("audio device entry is not an object");
            AudioDeviceAttributes attributes = audioDeviceAttributes(device);
            if (attributes == null) continue;
            String key = audioDeviceKey(attributes);
            if ("out".equals(device.optString("type"))) wantedOutputs.put(key, attributes);
            else wantedInputs.put(key, attributes);
        }
        reconcileAudioDirection(connectedAudioOutputs, wantedOutputs);
        reconcileAudioDirection(connectedAudioInputs, wantedInputs);
        updatePreferredMediaOutput();
    }

    private void reconcileAudioDirection(LinkedHashMap<String, AudioDeviceAttributes> current,
            LinkedHashMap<String, AudioDeviceAttributes> wanted) {
        for (Map.Entry<String, AudioDeviceAttributes> entry : new ArrayList<>(current.entrySet())) {
            AudioDeviceAttributes replacement = wanted.get(entry.getKey());
            if (replacement == null || !replacement.getName().equals(entry.getValue().getName())) {
                changeAudioConnection(entry.getValue(), false);
            }
        }
        for (AudioDeviceAttributes attributes : wanted.values()) changeAudioConnection(attributes, true);
    }

    private AudioDeviceAttributes audioDeviceAttributes(JSONObject device) throws JSONException {
        String type = device.optString("type", "");
        String address = device.optString("address", "");
        String description = device.optString("description", "");
        if (address.isEmpty() || description.isEmpty()) {
            throw new JSONException("audio device has an empty address or description");
        }
        if ("out".equals(type)) {
            return new AudioDeviceAttributes(AudioSystem.DEVICE_OUT_IP, address, description);
        }
        if ("in".equals(type)) {
            return new AudioDeviceAttributes(AudioSystem.DEVICE_IN_IP, address, description);
        }
        Log.e(TAG, "Unknown audio device direction=" + type + " address=" + address);
        return null;
    }

    private synchronized void changeAudioConnection(AudioDeviceAttributes attributes, boolean connected) {
        LinkedHashMap<String, AudioDeviceAttributes> devices =
                attributes.getRole() == AudioDeviceAttributes.ROLE_INPUT
                        ? connectedAudioInputs : connectedAudioOutputs;
        String key = audioDeviceKey(attributes);
        AudioDeviceAttributes current = devices.get(key);
        if (connected && current != null && current.getName().equals(attributes.getName())) return;
        if (!connected && current == null) return;
        AudioManager audioManager = getSystemService(AudioManager.class);
        if (audioManager == null) {
            Log.e(TAG, "Cannot change audio device connection: AudioManager unavailable");
            return;
        }
        try {
            if (connected) {
                audioManager.setWiredDeviceConnectionState(attributes, AudioSystem.DEVICE_STATE_AVAILABLE);
                devices.put(key, attributes);
                Log.i(TAG, "Connected PipeWire audio port=" + attributes);
            } else {
                AudioDeviceAttributes disconnect = current == null ? attributes : current;
                audioManager.setWiredDeviceConnectionState(disconnect,
                        AudioSystem.DEVICE_STATE_UNAVAILABLE);
                devices.remove(key);
                Log.i(TAG, "Disconnected PipeWire audio port=" + disconnect);
            }
            if (devices == connectedAudioOutputs) updatePreferredMediaOutput();
        } catch (RuntimeException e) {
            Log.e(TAG, "Audio device connection update failed: connected=" + connected
                    + " device=" + attributes, e);
        }
    }

    private synchronized void updatePreferredMediaOutput() {
        AudioManager audioManager = getSystemService(AudioManager.class);
        if (audioManager == null) return;
        AudioProductStrategy mediaStrategy = AudioProductStrategy.getAudioProductStrategyForAudioAttributes(
                new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build(), true);
        if (mediaStrategy == null) {
            Log.e(TAG, "Cannot find AudioPolicy media strategy for PipeWire routing");
            return;
        }
        AudioDeviceAttributes selected = null;
        for (AudioDeviceAttributes output : connectedAudioOutputs.values()) selected = output;
        try {
            if (selected == null) {
                audioManager.removePreferredDeviceForStrategy(mediaStrategy);
            } else if (!audioManager.setPreferredDeviceForStrategy(mediaStrategy, selected)) {
                Log.e(TAG, "AudioPolicy rejected preferred media device=" + selected);
            }
        } catch (RuntimeException e) {
            Log.e(TAG, "Cannot update AudioPolicy preferred media device=" + selected, e);
        }
    }

    private static String audioDeviceKey(AudioDeviceAttributes attributes) {
        return attributes.getRole() + ":" + attributes.getAddress();
    }

    private void rebindAudioSubscriptions() {
        if (!audioSubscriptionReady && internalAudioBridge != null) {
            internalAudioBridge.refresh();
            tryStartAudioSubscription();
        }
        for (Subscription subscription : new ArrayList<>(subscriptions.values())) {
            if ("audio".equals(subscription.target)) subscription.rebindIfNeeded();
        }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    private NetworkSpeakerDiscovery networkSpeakers;

    @Override public void onCreate() {
        super.onCreate();
        networkSpeakers = new NetworkSpeakerDiscovery(this);
        networkSpeakers.start();
        activeService = this;
        // sleep: poll off the main thread; failed power inspection keeps sleep blocked.
        sleepWakeForwarder = new AndroidWakeStateForwarder(this);
        sleepWakeForwarder.start();
        displayChannel = new DisplayChannel(this);
        trustChannel = new TrustChannel(this);
        startAudioDeviceForwarding();
        audioChannelHandler.postDelayed(audioChannelRebinder, 3000);
        wifiCardSwitcher = new WifiCardSwitcher(this, this::channelFor);
        wifiCardSwitcher.start();
        bluetoothControllerSwitcher = new BluetoothControllerSwitcher(this, this::channelFor);
        bluetoothControllerSwitcher.start();
        // The bridge is bound explicitly by apps. Root/adb administration is exposed
        // through the UID-gated ContentProvider below, never ServiceManager.
    }

    @Override public void onDestroy() {
        if (networkSpeakers != null) networkSpeakers.stop();
        // sleep: stop the poller with the persistent bridge service.
        if (sleepWakeForwarder != null) sleepWakeForwarder.stop();
        audioChannelHandler.removeCallbacks(audioChannelRebinder);
        if (wifiCardSwitcher != null) wifiCardSwitcher.stop();
        if (bluetoothControllerSwitcher != null) bluetoothControllerSwitcher.stop();
        if (internalAudioClient != null && internalAudioDeviceListener != null)
            internalAudioClient.unsubscribe(internalAudioDeviceListener);
        if (internalAudioBridge != null) internalAudioBridge.close();
        if (activeService == this) activeService = null;
        super.onDestroy();
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
            PackageInfo info = getPackageManager().getPackageInfoAsUser(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES, UserHandle.getUserId(uid));
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
        // The persistent bridge's private audio forwarder uses the same client
        // library as MatonOS apps; this self-call cannot grant access to another UID.
        if (uid == Process.myUid() && getPackageName().equals(packageName)
                && "audio".equals(target)) return packageName;
        try {
            PackageInfo info = getPackageManager().getPackageInfoAsUser(packageName,
                    PackageManager.GET_SIGNING_CERTIFICATES, UserHandle.getUserId(uid));
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
        return new HashSet<>(java.util.Arrays.asList("input", "sleep", "wifi", "bluetooth", "audio",
                "camera", "install", "addons", "status", "display", "trust", "hardware"));
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
        if (DisplayChannel.TARGET.equals(target)) return displayChannel;
        if (TrustChannel.TARGET.equals(target)) return trustChannel;
        if (HardwareChannel.TARGET.equals(target)) return hardwareChannel;
        // checkService is deliberately nonblocking: optional hardware instances may be absent.
        IBinder service = ServiceManager.checkService("vendor.matonos.channel.IChannel/" + target);
        return IChannel.Stub.asInterface(service);
    }

    private boolean channelAvailable(String target) {
        return channelFor(target) != null;
    }

    private final class Subscription {
        final String key;
        final String target;
        final String topic;
        final IMatonosListener appListener;
        volatile IChannel channel;
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
            IChannel current = channel;
            if (current == null) return;
            try { current.unsubscribe(topic, channelListener); }
            catch (RemoteException e) { Log.w(TAG, "Channel unsubscribe failed", e); }
        }
        void rebindIfNeeded() {
            IChannel latest = channelFor(target);
            IChannel previous = channel;
            if (latest == null) {
                if (previous == null) return;
                channel = null;
                try { previous.unsubscribe(topic, channelListener); }
                catch (RemoteException e) { Log.w(TAG, "Audio channel stop unsubscribe failed", e); }
                dispatchAudioSnapshot("{\"devices\":[]}");
                return;
            }
            if (previous != null && previous.asBinder().equals(latest.asBinder())) return;
            if (previous != null) {
                try { previous.unsubscribe(topic, channelListener); }
                catch (RemoteException e) { Log.w(TAG, "Audio channel restart unsubscribe failed", e); }
            }
            channel = latest;
            try {
                latest.subscribe(topic, channelListener);
                dispatchAudioSnapshot(latest.call("devices", "{}"));
                Log.i(TAG, "Rebound audio device subscription after HAL restart");
            } catch (Exception e) {
                channel = null;
                Log.e(TAG, "Audio channel rebind failed", e);
            }
        }
        private void dispatchAudioSnapshot(String json) {
            try {
                JSONObject snapshot = new JSONObject(json == null ? "{}" : json);
                JSONArray devices = snapshot.optJSONArray("devices");
                if (devices == null) devices = new JSONArray();
                JSONObject event = new JSONObject();
                event.put("event", "snapshot");
                event.put("devices", devices);
                appListener.onEvent(target, topic, event.toString());
            } catch (Exception e) {
                Log.e(TAG, "Cannot dispatch audio device resync snapshot", e);
            }
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

    /**
     * The "trust" target: lists which apps may use which bridge services (built-in and
     * developer-approved) for MatonOS Settings, and revokes developer trust.
     */
    static final class TrustChannel extends vendor.matonos.channel.IChannel.Stub {
        static final String TARGET = "trust";
        private static final String REQUESTS = "matonos_allow_bridge_trust_requests";
        private final android.content.Context context;

        TrustChannel(android.content.Context context) { this.context = context; }

        @Override public int getInterfaceVersion() { return vendor.matonos.channel.IChannel.VERSION; }
        @Override public String getInterfaceHash() { return vendor.matonos.channel.IChannel.HASH; }

        @Override public String call(String command, String jsonArgs) {
            long token = android.os.Binder.clearCallingIdentity();
            try {
                JSONObject args = new JSONObject(jsonArgs == null ? "{}" : jsonArgs);
                switch (command) {
                    case "get_state": return state().toString();
                    case "revoke": return revoke(args.getString("package"));
                    case "set_allow_requests": {
                        boolean allow = args.getBoolean("allow");
                        Settings.Global.putInt(context.getContentResolver(), REQUESTS, allow ? 1 : 0);
                        Log.i(TAG, "Bridge trust requests " + (allow ? "allowed" : "blocked"));
                        return allow ? "\"Apps may ask for trust\"" : "\"Trust requests are blocked\"";
                    }
                    default: throw new IllegalArgumentException("unknown trust command: " + command);
                }
            } catch (org.json.JSONException e) {
                throw new IllegalArgumentException("invalid trust arguments for " + command, e);
            } finally {
                android.os.Binder.restoreCallingIdentity(token);
            }
        }

        private JSONObject state() throws org.json.JSONException {
            java.util.Map<String, java.util.TreeSet<String>> builtin = new java.util.TreeMap<>();
            for (String line : readLines(context, R.raw.target_caller_allowlist)) {
                String[] fields = line.trim().split("\\s+");
                if (fields.length != 2) continue;
                builtin.computeIfAbsent(fields[1], k -> new java.util.TreeSet<>()).add(fields[0]);
            }
            JSONArray apps = new JSONArray();
            for (java.util.Map.Entry<String, java.util.TreeSet<String>> entry : builtin.entrySet()) {
                apps.put(app(entry.getKey(), true, entry.getValue()));
            }
            android.content.SharedPreferences prefs = context.getSharedPreferences(
                    "trusted_developer_apps", MODE_PRIVATE);
            for (java.util.Map.Entry<String, ?> entry : new java.util.TreeMap<>(prefs.getAll()).entrySet()) {
                java.util.TreeSet<String> targets = new java.util.TreeSet<>();
                try {
                    JSONArray values = new JSONObject((String) entry.getValue()).getJSONArray("targets");
                    for (int i = 0; i < values.length(); i++) targets.add(values.getString(i));
                } catch (Exception e) {
                    Log.e(TAG, "Unreadable trust entry for " + entry.getKey(), e);
                }
                apps.put(app(entry.getKey(), false, targets));
            }
            JSONObject state = new JSONObject();
            state.put("allowRequests", Settings.Global.getInt(context.getContentResolver(), REQUESTS, 0) == 1);
            state.put("apps", apps);
            return state;
        }

        private JSONObject app(String packageName, boolean builtin, Set<String> targets)
                throws org.json.JSONException {
            JSONObject app = new JSONObject();
            app.put("package", packageName);
            app.put("builtin", builtin);
            app.put("targets", new JSONArray(targets));
            PackageManager pm = context.getPackageManager();
            try {
                app.put("label", pm.getApplicationLabel(pm.getApplicationInfo(packageName, 0)).toString());
                app.put("installed", true);
            } catch (PackageManager.NameNotFoundException e) {
                app.put("label", packageName);
                app.put("installed", false);
            }
            return app;
        }

        private String revoke(String packageName) {
            android.content.SharedPreferences prefs = context.getSharedPreferences(
                    "trusted_developer_apps", MODE_PRIVATE);
            if (!prefs.contains(packageName))
                throw new IllegalArgumentException(packageName + " has no developer trust to revoke");
            prefs.edit().remove(packageName).apply();
            try {
                PackageInfo info = context.getPackageManager().getPackageInfo(packageName,
                        PackageManager.GET_SIGNING_CERTIFICATES);
                android.content.pm.Signature[] signers = info.signingInfo.getApkContentsSigners();
                String cert = signers.length == 1 ? sha256(signers[0].toByteArray()) : "";
                revokePermissionUnlessBuiltin(context, packageName, info.applicationInfo.uid, cert);
            } catch (PackageManager.NameNotFoundException e) {
                Log.i(TAG, "Revoked trust for uninstalled " + packageName);
            }
            Log.i(TAG, "Developer trust revoked for " + packageName);
            return "\"Trust revoked\"";
        }

        @Override public void subscribe(String topic, vendor.matonos.channel.IChannelListener listener) {
            throw new IllegalArgumentException("trust target has no topics");
        }
        @Override public void unsubscribe(String topic, vendor.matonos.channel.IChannelListener listener) { }
        @Override public String stageUpdate(String[] names, android.os.ParcelFileDescriptor[] files) {
            throw new UnsupportedOperationException("trust target has no updates");
        }
        @Override public String finalizeUpdate() {
            throw new UnsupportedOperationException("trust target has no updates");
        }
        @Override public String getUpdateStatus() {
            throw new UnsupportedOperationException("trust target has no updates");
        }
    }
}
