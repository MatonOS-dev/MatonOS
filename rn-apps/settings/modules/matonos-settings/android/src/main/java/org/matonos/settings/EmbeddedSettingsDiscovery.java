package org.matonos.settings;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.util.Base64;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class EmbeddedSettingsDiscovery {
    private static final String TAG = "MatonEmbeddedSettings";
    private static final String ACTION = "org.matonos.appsettings.EMBEDDED_SETTINGS";
    private static final String PERMISSION = "org.matonos.permission.BIND_EMBEDDED_SETTINGS";
    private static final String META_LABEL = "org.matonos.appsettings.label";
    private static final String META_ICON = "org.matonos.appsettings.icon";

    private EmbeddedSettingsDiscovery() {}

    public static List<Map<String, String>> list(Context context) {
        PackageManager pm = context.getPackageManager();
        ArrayList<Map<String, String>> entries = new ArrayList<>();
        try {
            List<ResolveInfo> results = pm.queryIntentServices(new Intent(ACTION),
                    PackageManager.ResolveInfoFlags.of(PackageManager.GET_META_DATA));
            for (ResolveInfo result : results) {
                ServiceInfo service = result.serviceInfo;
                if (service == null || !service.exported || !PERMISSION.equals(service.permission)) continue;
                CharSequence appLabel = service.applicationInfo.loadLabel(pm);
                String label = metadataLabel(pm, service, appLabel);
                Drawable icon = metadataIcon(pm, service);
                if (icon == null) icon = service.applicationInfo.loadIcon(pm);
                ComponentName component = new ComponentName(service.packageName, service.name);
                HashMap<String, String> entry = new HashMap<>();
                entry.put("component", component.flattenToString());
                entry.put("label", label);
                entry.put("package", service.packageName);
                entry.put("icon", encodeIcon(icon));
                entries.add(entry);
            }
            entries.sort(Comparator.comparing(item -> item.get("label"), String.CASE_INSENSITIVE_ORDER));
            return entries;
        } catch (RuntimeException e) {
            Log.e(TAG, "Embedded settings discovery failed action=" + ACTION, e);
            throw new IllegalStateException("Could not discover app settings", e);
        }
    }

    private static String metadataLabel(PackageManager pm, ServiceInfo service, CharSequence fallback) {
        Bundle data = service.metaData;
        if (data == null || !data.containsKey(META_LABEL)) return fallback.toString();
        Object value = data.get(META_LABEL);
        if (value instanceof Integer) {
            try { return pm.getText(service.packageName, (Integer) value, service.applicationInfo).toString(); }
            catch (RuntimeException e) { Log.e(TAG, "Invalid label metadata component=" + service.name, e); }
        } else if (value instanceof String && !((String) value).isEmpty()) {
            return (String) value;
        }
        Log.e(TAG, "Invalid label metadata component=" + service.name);
        return fallback.toString();
    }

    private static Drawable metadataIcon(PackageManager pm, ServiceInfo service) {
        Bundle data = service.metaData;
        if (data == null || !data.containsKey(META_ICON)) return null;
        Object value = data.get(META_ICON);
        if (value instanceof Integer) {
            try { return pm.getDrawable(service.packageName, (Integer) value, service.applicationInfo); }
            catch (RuntimeException e) { Log.e(TAG, "Invalid icon metadata component=" + service.name, e); }
        }
        Log.e(TAG, "Invalid icon metadata component=" + service.name);
        return null;
    }

    private static String encodeIcon(Drawable drawable) {
        try {
            int size = 96;
            Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            drawable.setBounds(0, 0, size, size);
            drawable.draw(new Canvas(bitmap));
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, bytes);
            bitmap.recycle();
            return "data:image/png;base64," + Base64.encodeToString(bytes.toByteArray(), Base64.NO_WRAP);
        } catch (RuntimeException e) {
            Log.e(TAG, "Could not encode app settings icon", e);
            return "";
        }
    }
}
