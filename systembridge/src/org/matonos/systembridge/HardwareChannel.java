package org.matonos.systembridge;

import android.os.SystemProperties;

import org.json.JSONException;
import org.json.JSONObject;

import vendor.matonos.channel.IChannel;
import vendor.matonos.channel.IChannelListener;

/**
 * The "hardware" target: what hardware the MatonOS daemons really found. Android's own
 * feature checks say yes for spoofed Wi-Fi/Bluetooth; the daemons publish the truth in
 * vendor.maton.<area>.* properties, which apps cannot read. A field is null until its
 * daemon has published a result.
 */
final class HardwareChannel extends IChannel.Stub {
    static final String TARGET = "hardware";

    @Override public int getInterfaceVersion() { return IChannel.VERSION; }
    @Override public String getInterfaceHash() { return IChannel.HASH; }

    @Override public String call(String command, String jsonArgs) {
        if ("security".equals(command)) return security();
        if (!"summary".equals(command)) throw new IllegalArgumentException("unknown hardware command: " + command);
        try {
            JSONObject result = new JSONObject();
            String wifiPresent = SystemProperties.get("vendor.maton.wifi.present");
            String wifiSelected = SystemProperties.get("vendor.maton.wifi.selected");
            boolean wifiVirtual = wifiSelected.startsWith("hwsim") || wifiSelected.startsWith("virt_wifi");
            result.put("wifi", new JSONObject()
                    .put("present", wifiPresent.isEmpty() ? JSONObject.NULL : "1".equals(wifiPresent) && !wifiVirtual)
                    .put("virtual", wifiPresent.isEmpty() ? JSONObject.NULL : wifiVirtual));
            String btPresent = SystemProperties.get("vendor.maton.bluetooth.present");
            result.put("bluetooth", new JSONObject()
                    .put("present", btPresent.isEmpty() ? JSONObject.NULL : "1".equals(btPresent))
                    .put("virtual", btPresent.isEmpty() ? JSONObject.NULL
                            : "1".equals(SystemProperties.get("vendor.maton.bluetooth.virtual"))));
            boolean audioDone = "1".equals(SystemProperties.get("vendor.maton.audio.selector_done"));
            result.put("audio", new JSONObject().put("present", audioDone
                    ? "found".equals(SystemProperties.get("vendor.maton.audio.selector_result")) : JSONObject.NULL));
            boolean gpuDone = !SystemProperties.get("vendor.maton.graphics.mode").isEmpty();
            boolean software = "true".equals(SystemProperties.get("vendor.maton.graphics.software"));
            result.put("gpu", new JSONObject()
                    .put("present", gpuDone ? !software : JSONObject.NULL)
                    .put("software", gpuDone ? software : JSONObject.NULL));
            return result.toString();
        } catch (JSONException e) {
            throw new IllegalStateException("hardware summary encoding failed", e);
        }
    }

    /** Read-only security status for the Settings Security page; values the Settings app
     * cannot read itself because it is not platform-signed. Empty properties are null. */
    private static String security() {
        try {
            JSONObject result = new JSONObject();
            result.put("selinux", !android.os.SELinux.isSELinuxEnabled() ? "Disabled"
                    : android.os.SELinux.isSELinuxEnforced() ? "Enforcing" : "Permissive");
            String cryptoState = SystemProperties.get("ro.crypto.state");
            String cryptoType = SystemProperties.get("ro.crypto.type");
            boolean metadata = SystemProperties.getBoolean("ro.crypto.metadata.enabled", false);
            result.put("encryption", cryptoState.isEmpty() ? JSONObject.NULL
                    : !"encrypted".equals(cryptoState) ? "Not encrypted"
                    : "file".equals(cryptoType) ? (metadata ? "Encrypted (file + metadata)" : "Encrypted (file)")
                    : "Encrypted (" + cryptoType + ")");
            String vendorPatch = SystemProperties.get("ro.vendor.build.security_patch");
            result.put("vendorSecurityPatch", vendorPatch.isEmpty() ? JSONObject.NULL : vendorPatch);
            String bootState = SystemProperties.get("ro.boot.verifiedbootstate");
            String deviceState = SystemProperties.get("ro.boot.vbmeta.device_state");
            result.put("verifiedBoot", bootState.isEmpty() ? JSONObject.NULL
                    : ("locked".equals(deviceState) ? "Locked" : deviceState.isEmpty() ? "Unknown lock state" : "Unlocked")
                    + ", " + ("green".equals(bootState) ? "verified" : bootState));
            return result.toString();
        } catch (JSONException e) {
            throw new IllegalStateException("security status encoding failed", e);
        }
    }

    @Override public void subscribe(String topic, IChannelListener listener) {
        throw new UnsupportedOperationException("hardware has no topics");
    }
    @Override public void unsubscribe(String topic, IChannelListener listener) { }
    @Override public String stageUpdate(String[] names, android.os.ParcelFileDescriptor[] files) {
        throw new UnsupportedOperationException("hardware does not stage updates");
    }
    @Override public String finalizeUpdate() { throw new UnsupportedOperationException("hardware does not stage updates"); }
    @Override public String getUpdateStatus() { throw new UnsupportedOperationException("hardware does not stage updates"); }
}
