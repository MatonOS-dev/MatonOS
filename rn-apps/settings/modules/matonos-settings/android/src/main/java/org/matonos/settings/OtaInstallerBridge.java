package org.matonos.settings;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import org.matonos.client.MatonosClient;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Java-only descriptor bridge for OTA files selected by Settings. */
public final class OtaInstallerBridge {
    private OtaInstallerBridge() {}

    public static Map<String, Object> stage(Context context, MatonosClient client,
            List<String> names, List<String> uriStrings) {
        if (names == null || uriStrings == null || names.size() != uriStrings.size())
            throw new IllegalArgumentException("OTA names and URIs must have equal lengths");
        List<ParcelFileDescriptor> opened = new ArrayList<>();
        try {
            for (String value : uriStrings) {
                Uri uri = Uri.parse(value);
                ParcelFileDescriptor descriptor = context.getContentResolver().openFileDescriptor(uri, "r");
                if (descriptor == null) throw new IllegalStateException("Cannot open OTA payload URI: " + uri);
                opened.add(descriptor);
            }
            MatonosClient.Result<String> result = client.stageUpdate(names.toArray(new String[0]),
                    opened.toArray(new ParcelFileDescriptor[0]));
            Map<String, Object> response = new HashMap<>();
            response.put("available", result.available);
            response.put("value", result.value == null ? "" : result.value);
            response.put("reason", result.reason == null ? "" : result.reason);
            return response;
        } catch (Exception e) {
            throw new IllegalStateException("Opening or staging OTA payload descriptors failed", e);
        } finally {
            for (ParcelFileDescriptor descriptor : opened) {
                try { descriptor.close(); }
                catch (Exception e) { android.util.Log.e("MatonOSSettings", "close OTA payload descriptor failed", e); }
            }
        }
    }
}
