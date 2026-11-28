package org.matonos.systembridge;

import android.app.AlarmManager;
import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.UserHandle;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiManager;
import android.os.UserManager;
import android.system.Os;
import android.util.Log;

import com.android.internal.widget.LockPatternUtils;
import com.android.internal.widget.LockscreenCredential;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Applies installer-provided first-boot settings, checkpointing each successful field. */
final class SetupApplier {
    private static final String TAG = "MatonSetup";
    private static final String SETUP_PATH = "/maton_system/setup/setup.json";
    private static final String CHANNEL = "setup";
    private static final int NOTIFICATION_ID = 2;

    private SetupApplier() { }

    static void apply(Context context) {
        File file = new File(SETUP_PATH);
        File pending = new File(SETUP_PATH + ".tmp");
        if (pending.exists()) {
            // A crash after a checkpoint's wipe but before its rename leaves the remaining
            // settings only in the temporary file.
            try {
                new JSONObject(java.nio.file.Files.readString(pending.toPath()));
                Os.rename(pending.getPath(), file.getPath());
                Log.w(TAG, "Recovered interrupted setup checkpoint " + pending);
            } catch (Exception error) {
                Log.e(TAG, "Unusable setup checkpoint " + pending + "; using " + SETUP_PATH, error);
                try { wipeAndDelete(pending); }
                catch (Exception wipeError) { Log.e(TAG, "Failed to remove " + pending, wipeError); }
            }
        }
        if (!file.exists()) return;
        JSONObject settings;
        try {
            settings = new JSONObject(java.nio.file.Files.readString(file.toPath()));
        } catch (Exception error) {
            Log.e(TAG, "Cannot parse setup file " + SETUP_PATH + "; retaining file", error);
            return;
        }
        if (settings.optInt("version", -1) != 1) {
            Log.e(TAG, "Unsupported setup version " + settings.opt("version")
                    + " in " + SETUP_PATH + "; retaining file");
            return;
        }
        // SetupV1 schema: version, locale, timeZone, wifi[], user{}, and services{linux:boolean}.
        for (String key : settings.keySet()) {
            if (!"version".equals(key) && !"locale".equals(key) && !"timeZone".equals(key)
                    && !"wifi".equals(key) && !"user".equals(key) && !"services".equals(key)
                    && !"account".equals(key)) {
                Log.e(TAG, "Unknown setup field " + key + " in " + SETUP_PATH + "; retaining file");
                return;
            }
        }
        try {
            if (settings.has("user")) {
                JSONObject user = settings.getJSONObject("user");
                requireKeys(user, "user", "name", "credential");
                if (user.has("credential")) {
                    requireKeys(user.getJSONObject("credential"), "user.credential", "type", "value");
                }
            }
            if (settings.has("services")) {
                requireKeys(settings.getJSONObject("services"), "services", "linux");
            }
            if (settings.has("account")) {
                requireKeys(settings.getJSONObject("account"), "account", "username", "refreshToken", "machineId");
            }
            if (settings.has("wifi")) {
                JSONArray networks = settings.getJSONArray("wifi");
                for (int i = 0; i < networks.length(); i++) {
                    requireKeys(networks.getJSONObject(i), "wifi[" + i + "]",
                            "ssid", "security", "password", "hidden");
                }
            }
        } catch (Exception error) {
            Log.e(TAG, "Invalid setup schema in " + SETUP_PATH + "; retaining file", error);
            return;
        }

        List<String> failed = new ArrayList<>();
        applyTopLevel(context, file, settings, "locale", failed, () -> {
            String tag = settings.getString("locale");
            Locale locale = Locale.forLanguageTag(tag);
            if (locale.getLanguage().isEmpty()) throw new IllegalArgumentException("invalid locale tag");
            ActivityManager manager = context.getSystemService(ActivityManager.class);
            if (manager == null) throw new IllegalStateException("ActivityManager unavailable");
            manager.setDeviceLocales(android.os.LocaleList.forLanguageTags(tag));
        });
        applyTopLevel(context, file, settings, "timeZone", failed, () -> {
            String zone = settings.getString("timeZone");
            if (!java.time.ZoneId.getAvailableZoneIds().contains(zone)) {
                throw new IllegalArgumentException("unknown time zone " + zone);
            }
            AlarmManager manager = context.getSystemService(AlarmManager.class);
            if (manager == null) throw new IllegalStateException("AlarmManager unavailable");
            manager.setTimeZone(zone);
        });
        applyTopLevel(context, file, settings, "wifi", failed, () -> {
            JSONArray networks = settings.getJSONArray("wifi");
            WifiManager manager = context.getSystemService(WifiManager.class);
            if (manager == null) throw new IllegalStateException("WifiManager unavailable");
            for (int i = 0; i < networks.length(); i++) {
                JSONObject network = networks.getJSONObject(i);
                WifiConfiguration config = new WifiConfiguration();
                config.SSID = quote(network.getString("ssid"));
                config.hiddenSSID = network.optBoolean("hidden", false);
                String security = network.getString("security");
                switch (security) {
                    case "open":
                        config.allowedKeyManagement.set(WifiConfiguration.KeyMgmt.NONE);
                        break;
                    case "wpa2":
                        config.allowedKeyManagement.set(WifiConfiguration.KeyMgmt.WPA_PSK);
                        config.preSharedKey = quote(network.getString("password"));
                        break;
                    case "wpa3":
                        config.allowedKeyManagement.set(WifiConfiguration.KeyMgmt.SAE);
                        config.preSharedKey = quote(network.getString("password"));
                        break;
                    default: throw new IllegalArgumentException("unsupported security " + security);
                }
                WifiManager.AddNetworkResult result = manager.addNetworkPrivileged(config);
                if (result.statusCode != WifiManager.AddNetworkResult.STATUS_SUCCESS) {
                    throw new IllegalStateException("addNetworkPrivileged failed with " + result.statusCode);
                }
            }
        });
        applyUser(context, file, settings, failed);
        applyAccount(context, file, settings, failed);
        applyTopLevel(context, file, settings, "services", failed, () -> {
            JSONObject services = settings.getJSONObject("services");
            if (!services.has("linux")) throw new JSONException("services.linux is required");
            if (!services.getBoolean("linux")) {
                Context userContext = context.createContextAsUser(UserHandle.SYSTEM, 0);
                PackageManager manager = userContext.getPackageManager();
                if (manager == null) throw new IllegalStateException("PackageManager unavailable for user 0");
                manager.setApplicationEnabledSetting("org.matonos.linux",
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED_USER, 0);
                Log.i(TAG, "Disabled org.matonos.linux for user 0 from setup.services.linux");
            }
        });

        if (settings.length() == 1) {
            try {
                wipeAndDelete(file);
            } catch (Exception error) {
                failed.add("cleanup");
                Log.e(TAG, "Failed to securely remove completed setup file " + SETUP_PATH, error);
            }
        }
        if (!failed.isEmpty()) postFailure(context, failed);
    }

    private interface FieldAction { void run() throws Exception; }

    private static void applyAccount(Context context, File file, JSONObject settings,
            List<String> failed) {
        if (!settings.has("account")) return;
        JSONObject account = settings.optJSONObject("account");
        try {
            if (account == null) throw new JSONException("account must be an object");
            String username = account.getString("username");
            String refreshToken = account.getString("refreshToken");
            String installerMachineId = account.getString("machineId");
            // The refresh token is bound to the machine id the installer signed in with; the
            // authenticator stores it with the account and sends it on every refresh.
            if (username.isBlank() || refreshToken.isBlank() || installerMachineId.isBlank())
                throw new IllegalArgumentException("account fields must not be blank");
            Intent intent = new Intent("org.matonos.settings.APPLY_SETUP_ACCOUNT")
                    .setPackage("org.matonos.settings")
                    .putExtra("username", username)
                    .putExtra("refresh_token", refreshToken)
                    .putExtra("machine_id", installerMachineId)
                    .addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES);
            context.sendBroadcastAsUser(intent, UserHandle.SYSTEM,
                    "org.matonos.permission.APPLY_SETUP_ACCOUNT");
        } catch (Exception error) {
            failed.add("account");
            Log.e(TAG, "Failed to apply first-boot MatonOS account; account data will be discarded", error);
        }
        // Credentials are deliberately one-shot. Even a failed account apply must not leave
        // a refresh token in setup.json for an unattended retry.
        settings.remove("account");
        try {
            checkpoint(file, settings);
        } catch (Exception error) {
            failed.add("account.cleanup");
            Log.e(TAG, "Failed to remove first-boot account data from " + SETUP_PATH, error);
        }
    }

    private static void requireKeys(JSONObject object, String field, String... allowed)
            throws JSONException {
        for (String key : object.keySet()) {
            boolean found = false;
            for (String candidate : allowed) if (candidate.equals(key)) found = true;
            if (!found) throw new JSONException("unknown key " + field + "." + key);
        }
    }

    private static void applyTopLevel(Context context, File file, JSONObject settings,
            String field, List<String> failed, FieldAction action) {
        if (!settings.has(field)) return;
        Object original = settings.opt(field);
        boolean removed = false;
        try {
            action.run();
            settings.remove(field);
            removed = true;
            checkpoint(file, settings);
        } catch (Exception error) {
            if (removed) {
                try { settings.put(field, original); }
                catch (Exception restoreError) {
                    Log.e(TAG, "Failed to restore setup field " + field + " in memory", restoreError);
                }
            }
            failed.add(field);
            Log.e(TAG, "Failed to apply setup field " + field + "; retaining it", error);
        }
    }

    private static void applyUser(Context context, File file, JSONObject settings,
            List<String> failed) {
        if (!settings.has("user")) return;
        try {
            JSONObject user = settings.getJSONObject("user");
            if (user.has("name")) {
                Object original = user.opt("name");
                boolean removed = false;
                try {
                    UserManager manager = context.getSystemService(UserManager.class);
                    if (manager == null) throw new IllegalStateException("UserManager unavailable");
                    manager.setUserName(0, user.getString("name"));
                    user.remove("name");
                    removed = true;
                    checkpoint(file, settings);
                } catch (Exception error) {
                    if (removed) {
                        try { user.put("name", original); }
                        catch (Exception restoreError) {
                            Log.e(TAG, "Failed to restore setup field user.name in memory", restoreError);
                        }
                    }
                    failed.add("user.name");
                    Log.e(TAG, "Failed to apply setup field user.name; retaining it", error);
                }
            }
            if (user.has("credential")) {
                Object original = user.opt("credential");
                boolean removed = false;
                try {
                    JSONObject credential = user.getJSONObject("credential");
                    String type = credential.getString("type");
                    String value = credential.getString("value");
                    LockscreenCredential lock;
                    if ("pin".equals(type)) lock = LockscreenCredential.createPin(value);
                    else if ("password".equals(type)) lock = LockscreenCredential.createPassword(value);
                    else throw new IllegalArgumentException("unsupported credential type " + type);
                    try {
                        if (!new LockPatternUtils(context).setLockCredential(
                                lock, LockscreenCredential.createNone(), 0)) {
                            throw new IllegalStateException("setLockCredential returned false");
                        }
                    } finally {
                        lock.zeroize();
                    }
                    user.remove("credential");
                    removed = true;
                    checkpoint(file, settings);
                } catch (Exception error) {
                    if (removed) {
                        try { user.put("credential", original); }
                        catch (Exception restoreError) {
                            Log.e(TAG, "Failed to restore setup field user.credential in memory",
                                    restoreError);
                        }
                    }
                    failed.add("user.credential");
                    Log.e(TAG, "Failed to apply setup field user.credential; value redacted ("
                            + error.getClass().getSimpleName() + ")");
                }
            }
            if (user.length() == 0) settings.remove("user");
            checkpoint(file, settings);
        } catch (Exception error) {
            failed.add("user");
            Log.e(TAG, "Failed to parse setup field user; retaining it", error);
        }
    }

    private static String quote(String value) { return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\""; }

    private static void checkpoint(File file, JSONObject settings) throws Exception {
        File temporary = new File(file.getParentFile(), file.getName() + ".tmp");
        byte[] data = settings.toString().getBytes(StandardCharsets.UTF_8);
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            output.write(data);
            output.getFD().sync();
        }
        // The replaced file may hold secrets; zero its blocks before the rename frees them.
        zeroInPlace(file);
        Os.rename(temporary.getPath(), file.getPath());
    }

    /** Overwrites the file's existing blocks without truncating, so the old data is gone. */
    private static void zeroInPlace(File file) throws Exception {
        try (java.io.RandomAccessFile output = new java.io.RandomAccessFile(file, "rw")) {
            long length = output.length();
            byte[] zeros = new byte[8192];
            output.seek(0);
            while (length > 0) {
                int count = (int) Math.min(length, zeros.length);
                output.write(zeros, 0, count);
                length -= count;
            }
            output.getFD().sync();
        }
    }

    private static void wipeAndDelete(File file) throws Exception {
        zeroInPlace(file);
        if (!file.delete()) throw new java.io.IOException("delete failed for " + file);
    }

    private static void postFailure(Context context, List<String> failed) {
        try {
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            if (manager == null) throw new IllegalStateException("NotificationManager unavailable");
            manager.createNotificationChannel(new NotificationChannel(
                    CHANNEL, "Setup", NotificationManager.IMPORTANCE_DEFAULT));
            String message = "Some setup settings could not be applied: " + String.join(", ", failed);
            Notification notification = new Notification.Builder(context, CHANNEL)
                    .setSmallIcon(android.R.drawable.stat_sys_warning)
                    .setContentTitle("Setup incomplete")
                    .setContentText(message)
                    .setStyle(new Notification.BigTextStyle().bigText(message))
                    .setAutoCancel(false)
                    .build();
            manager.notify(NOTIFICATION_ID, notification);
        } catch (RuntimeException error) {
            Log.e(TAG, "Failed to post setup failure notification for " + failed, error);
        }
    }
}
