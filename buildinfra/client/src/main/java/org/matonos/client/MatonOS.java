package org.matonos.client;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.provider.Settings;

import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

/** Application-level MatonOS initialization, startup checks, and trust requests. */
public final class MatonOS {
    public static final String PERMISSION = "org.matonos.permission.SYSTEM_BRIDGE";
    public static final String TRUST_SETTINGS = "org.matonos.systembridge.TRUST_SETTINGS";
    public static final String TRUST_REQUESTS_SETTING = "matonos_allow_bridge_trust_requests";
    private static final String TRUST_ACTION_PREFIX = "org.matonos.systembridge.TRUST_REQUEST";

    public interface ReadyCallback { void onReady(MatonosClient client); }
    public interface TrustCallback { void onResult(TrustOutcome outcome); }
    public enum TrustStatus { GRANTED, DENIED, UNAVAILABLE }
    public static final class TrustOutcome {
        public final TrustStatus status;
        public final String reason;
        TrustOutcome(TrustStatus status, String reason) { this.status = status; this.reason = reason; }
    }
    public static final class TrustRequest {
        private final Activity activity;
        private final String action;
        private final AtomicBoolean complete = new AtomicBoolean();
        private volatile TrustOutcome outcome;
        private volatile TrustCallback callback;
        private BroadcastReceiver receiver;
        TrustRequest(Activity activity, String action) { this.activity = activity; this.action = action; }
        void finish(TrustOutcome value) {
            if (!complete.compareAndSet(false, true)) return;
            outcome = value;
            if (receiver != null) try { activity.unregisterReceiver(receiver); } catch (Exception ignored) { }
            if (value.status == TrustStatus.GRANTED) MatonosClient.refreshAll();
            TrustCallback current = callback;
            if (current != null) current.onResult(value);
        }
        public TrustRequest onResult(TrustCallback callback) {
            this.callback = callback;
            TrustOutcome current = outcome;
            if (current != null && callback != null) callback.onResult(current);
            return this;
        }
    }

    private MatonOS() { }

    public static MatonosClient init(Context context, BridgeMode mode) {
        return new MatonosClient(context, mode);
    }
    public static MatonosClient init(Context context) {
        return new MatonosClient(context, modeFromManifest(context));
    }
    static BridgeMode modeFromManifest(Context context) {
        try {
            ApplicationInfo info = context.getPackageManager().getApplicationInfo(
                    context.getPackageName(), android.content.pm.PackageManager.GET_META_DATA);
            String value = info.metaData == null ? null : info.metaData.getString("org.matonos.bridgeMode");
            return "optional".equalsIgnoreCase(value) ? BridgeMode.OPTIONAL : BridgeMode.REQUIRED;
        } catch (Exception ignored) { return BridgeMode.REQUIRED; }
    }

    public static MatonosClient requireBridge(Activity activity, String[] accessTargets,
                                               String[] requiredChannelTargets, ReadyCallback ready) {
        MatonosClient client = init(activity, BridgeMode.REQUIRED);
        client.checkStartup(accessTargets, requiredChannelTargets, result -> activity.runOnUiThread(() -> {
            if (activity.isFinishing() || activity.isDestroyed()) return;
            if (result.available) { if (ready != null) ready.onReady(client); return; }
            showRequiredFailure(activity, client, accessTargets, requiredChannelTargets, ready, result.reason);
        }));
        return client;
    }

    private static void showRequiredFailure(Activity activity, MatonosClient client,
                                            String[] accessTargets, String[] channels,
                                            ReadyCallback ready, String reason) {
        boolean trustButton = ("BRIDGE_APP_NOT_TRUSTED".equals(reason)
                || "BRIDGE_PERMISSION_REQUIRED".equals(reason))
                && trustRequestsEnabled(activity);
        int title = R.string.matonos_bridge_required_title;
        String message = failureMessage(activity, reason, accessTargets, channels);
        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle(title).setMessage(message).setCancelable(false);
        builder.setPositiveButton(trustButton ? R.string.matonos_trust_this_app : R.string.matonos_exit,
                (dialog, which) -> {
                    if (!trustButton) { client.close(); activity.finish(); return; }
                    requestTrust(activity, accessTargets, outcome -> {
                        if (activity.isFinishing()) return;
                        if (outcome.status == TrustStatus.GRANTED) {
                            client.checkStartup(accessTargets, channels, check -> activity.runOnUiThread(() -> {
                                if (check.available) { if (ready != null) ready.onReady(client); }
                                else showRequiredFailure(activity, client, accessTargets, channels, ready, check.reason);
                            }));
                        } else showRequiredFailure(activity, client, accessTargets, channels, ready,
                                outcome.reason == null ? "BRIDGE_APP_NOT_TRUSTED" : outcome.reason);
                    });
                });
        if (trustButton) builder.setNegativeButton(R.string.matonos_exit,
                (dialog, which) -> { client.close(); activity.finish(); });
        builder.show();
    }

    private static String failureMessage(Context context, String reason, String[] accessTargets, String[] channels) {
        if (reason == null) reason = "BRIDGE_UNAVAILABLE";
        switch (reason) {
            case "BRIDGE_NOT_INSTALLED": return context.getString(R.string.matonos_needs_matonos);
            case "BRIDGE_UPDATE_REQUIRED": return context.getString(R.string.matonos_update_matonos);
            case "BRIDGE_PERMISSION_REQUIRED": return context.getString(R.string.matonos_permission_missing);
            case "BRIDGE_APP_NOT_TRUSTED":
                String targets = accessTargets == null ? "" : String.join(" ", accessTargets);
                return context.getString(R.string.matonos_developer_not_trusted,
                        context.getPackageName(), targets);
            default:
                if (reason.startsWith("CHANNEL_UNAVAILABLE:"))
                    return context.getString(R.string.matonos_channel_missing, reason.substring(reason.indexOf(':') + 1));
                if (reason.startsWith("TRUST_REQUESTS_DISABLED"))
                    return context.getString(R.string.matonos_trust_disabled, context.getPackageName());
                return context.getString(R.string.matonos_bridge_unavailable);
        }
    }

    public static boolean trustRequestsEnabled(Context context) {
        return Settings.Global.getInt(context.getContentResolver(), Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) == 1
                && Settings.Global.getInt(context.getContentResolver(), TRUST_REQUESTS_SETTING, 0) == 1;
    }

    /** Opens the user-confirmed trust screen and returns a handle for the outcome callback. */
    public static TrustRequest requestTrust(Activity activity, String... targets) {
        String action = TRUST_ACTION_PREFIX + "." + UUID.randomUUID().toString().replace("-", "");
        TrustRequest request = new TrustRequest(activity, action);
        if (Settings.Global.getInt(activity.getContentResolver(), Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) != 1) {
            request.finish(new TrustOutcome(TrustStatus.UNAVAILABLE, "DEVELOPER_OPTIONS_DISABLED")); return request;
        }
        if (!trustRequestsEnabled(activity)) {
            request.finish(new TrustOutcome(TrustStatus.UNAVAILABLE,
                    "TRUST_REQUESTS_DISABLED (enable the option in Trusted developer apps, or run: adb shell content call --uri content://org.matonos.systembridge.shell --method trust --arg "
                            + activity.getPackageName() + " --extra targets:s:" + String.join(",", targets == null ? new String[0] : targets) + ")"));
            return request;
        }
        String[] requested = targets == null ? new String[0] : targets;
        Intent intent = new Intent(TRUST_SETTINGS).setComponent(new ComponentName(
                "org.matonos.systembridge", "org.matonos.systembridge.SystemBridgeService$TrustedAppsActivity"));
        intent.putExtra("requestedPackage", activity.getPackageName());
        intent.putExtra("requestedTargets", requested);
        intent.putExtra("callbackAction", action);
        intent.putExtra("callbackPackage", activity.getPackageName());
        String nonce = UUID.randomUUID().toString();
        intent.putExtra("callbackNonce", nonce);
        request.receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent result) {
                if (!nonce.equals(result.getStringExtra("callbackNonce"))) return;
                String status = result.getStringExtra("trustStatus");
                TrustStatus parsed;
                try { parsed = TrustStatus.valueOf(status); }
                catch (Exception e) { parsed = TrustStatus.UNAVAILABLE; }
                request.finish(new TrustOutcome(parsed, result.getStringExtra("trustReason")));
            }
        };
        IntentFilter filter = new IntentFilter(action);
        if (Build.VERSION.SDK_INT >= 33) activity.registerReceiver(request.receiver, filter, Context.RECEIVER_EXPORTED);
        else activity.registerReceiver(request.receiver, filter);
        try {
            activity.startActivityForResult(intent, 0x4d54);
        } catch (Exception e) {
            request.finish(new TrustOutcome(TrustStatus.UNAVAILABLE, "BRIDGE_NOT_INSTALLED"));
        }
        return request;
    }

    public static TrustRequest requestTrust(Activity activity, String[] targets, TrustCallback callback) {
        return requestTrust(activity, targets).onResult(callback);
    }
}
