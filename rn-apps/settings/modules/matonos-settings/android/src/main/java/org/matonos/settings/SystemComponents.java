package org.matonos.settings;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.provider.Settings;
import android.util.Log;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Candidate discovery and secure selection for replaceable caption and tray providers. */
public final class SystemComponents {
    private static final String TAG = "MatonSystemComponents";
    private static final String CAPTION_ACTION = "org.matonos.caption.PROVIDER";
    private static final String CAPTION_PERMISSION = "org.matonos.permission.BIND_CAPTION_PROVIDER";
    private static final String TRAY_ACTION = "org.matonos.tray.HOST";
    private static final String TRAY_PERMISSION = "org.matonos.permission.BIND_TRAY_HOST";

    private SystemComponents() {}

    public static Map<String, Object> read(Context context) {
        HashMap<String, Object> result = new HashMap<>();
        result.put("captions", readKind(context, "captions"));
        result.put("tray", readKind(context, "tray"));
        return result;
    }

    private static Map<String, Object> readKind(Context context, String kind) {
        String setting = settingFor(kind);
        HashMap<String, Object> result = new HashMap<>();
        String current = Settings.Secure.getString(context.getContentResolver(), setting);
        result.put("current", current == null ? "" : current);
        result.put("candidates", candidates(context, kind));
        return result;
    }

    private static List<Map<String, String>> candidates(Context context, String kind) {
        String action = actionFor(kind);
        String requiredPermission = permissionFor(kind);
        PackageManager packageManager = context.getPackageManager();
        ArrayList<Map<String, String>> result = new ArrayList<>();
        try {
            Intent intent = new Intent(action);
            List<ResolveInfo> services = packageManager.queryIntentServices(
                    intent, PackageManager.ResolveInfoFlags.of(0));
            for (ResolveInfo resolveInfo : services) {
                ServiceInfo service = resolveInfo.serviceInfo;
                if (service == null || !requiredPermission.equals(service.permission)) continue;
                ComponentName component = new ComponentName(service.packageName, service.name);
                HashMap<String, String> candidate = new HashMap<>();
                candidate.put("component", component.flattenToString());
                candidate.put("label", service.loadLabel(packageManager).toString());
                candidate.put("package", service.packageName);
                result.add(candidate);
            }
        } catch (RuntimeException e) {
            Log.e(TAG, "Candidate discovery failed kind=" + kind + " action=" + action, e);
            throw new IllegalStateException("Could not discover " + kind + " providers", e);
        }
        return result;
    }

    public static boolean write(Context context, String kind, String flattened) {
        String setting = settingFor(kind);
        String value = flattened == null || flattened.isEmpty() ? null : flattened;
        if (value != null) {
            ComponentName requested = ComponentName.unflattenFromString(value);
            if (requested == null || candidates(context, kind).stream().noneMatch(candidate ->
                    requested.flattenToString().equals(candidate.get("component")))) {
                Log.e(TAG, "Refusing invalid provider selection kind=" + kind
                        + " component=" + flattened);
                throw new IllegalArgumentException("Provider is not an installed candidate");
            }
            value = requested.flattenToString();
        }
        try {
            boolean written = Settings.Secure.putString(context.getContentResolver(), setting, value);
            if (!written) {
                Log.e(TAG, "Secure setting write returned false kind=" + kind
                        + " setting=" + setting + " value=" + value);
                throw new IllegalStateException("Could not save provider selection");
            }
            return true;
        } catch (SecurityException | IllegalArgumentException e) {
            Log.e(TAG, "Secure setting write failed kind=" + kind
                    + " setting=" + setting + " value=" + value, e);
            throw e;
        }
    }

    private static String settingFor(String kind) {
        switch (kind) {
            case "captions": return "matonos_caption_provider";
            case "tray": return "matonos_tray_host";
            default:
                Log.e(TAG, "Unknown system component kind=" + kind);
                throw new IllegalArgumentException("Unknown system component kind");
        }
    }

    private static String actionFor(String kind) {
        switch (kind) {
            case "captions": return CAPTION_ACTION;
            case "tray": return TRAY_ACTION;
            default: settingFor(kind); throw new AssertionError("unreachable");
        }
    }

    private static String permissionFor(String kind) {
        switch (kind) {
            case "captions": return CAPTION_PERMISSION;
            case "tray": return TRAY_PERMISSION;
            default: settingFor(kind); throw new AssertionError("unreachable");
        }
    }
}
