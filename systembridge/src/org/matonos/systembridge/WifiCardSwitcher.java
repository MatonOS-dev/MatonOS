package org.matonos.systembridge;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

import vendor.matonos.channel.IChannel;
import vendor.matonos.channel.IChannelListener;

/** Serializes Wi-Fi card selection around Android's single wlan0 interface. */
final class WifiCardSwitcher {
    private static final String TAG = "MatonSystemBridge";
    interface ChannelProvider { IChannel channelFor(String target); }

    private final WifiManager wifi;
    private final ChannelProvider channels;
    private final ScheduledExecutorService executor = Executors.newSingleThreadScheduledExecutor();
    private volatile IChannel subscribedChannel;
    private final IChannelListener listener = new IChannelListener.Stub() {
        @Override public int getInterfaceVersion() { return IChannelListener.VERSION; }
        @Override public String getInterfaceHash() { return IChannelListener.HASH; }
        @Override public void onEvent(String topic, String json) {
            if ("devices_changed".equals(topic)) executor.execute(this::reconcileSafely);
        }
        private void reconcileSafely() { reconcile(); }
    };

    WifiCardSwitcher(Context context, ChannelProvider provider) {
        wifi = context.getSystemService(WifiManager.class);
        channels = provider;
    }

    void start() { executor.scheduleWithFixedDelay(this::bindAndReconcile, 0, 3, TimeUnit.SECONDS); }
    void stop() {
        IChannel channel = subscribedChannel;
        subscribedChannel = null;
        if (channel != null) try { channel.unsubscribe("devices_changed", listener); }
        catch (Exception e) { Log.e(TAG, "Wi-Fi devices_changed unsubscribe failed", e); }
        executor.shutdownNow();
    }

    String switchTo(String permMac, boolean persist) {
        if (permMac == null || permMac.isEmpty()) throw new IllegalStateException("Wi-Fi card MAC is required");
        Future<String> future = executor.submit(() -> switchNow(permMac, persist));
        try { return future.get(30, TimeUnit.SECONDS); }
        catch (TimeoutException e) { future.cancel(true); Log.e(TAG, "Wi-Fi switch timed out for MAC=" + permMac, e); throw new IllegalStateException("Wi-Fi switch timed out for " + permMac, e); }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); Log.e(TAG, "Wi-Fi switch interrupted for MAC=" + permMac, e); throw new IllegalStateException("Wi-Fi switch interrupted for " + permMac, e); }
        catch (ExecutionException e) { Throwable cause = e.getCause(); if (cause instanceof IllegalStateException) throw (IllegalStateException) cause; Log.e(TAG, "Wi-Fi switch failed for MAC=" + permMac, cause); throw new IllegalStateException("Wi-Fi switch failed for " + permMac, cause); }
    }

    private String switchNow(String permMac, boolean persist) {
        boolean wasEnabled = wifi != null && wifi.isWifiEnabled();
        Log.i(TAG, "Wi-Fi switch begin MAC=" + permMac + " persist=" + persist + " enabled=" + wasEnabled);
        Exception failure = null;
        String result = null;
        try {
            if (wifi == null) throw new IllegalStateException("WifiManager unavailable");
            if (wasEnabled) {
                Log.i(TAG, "Disabling Wi-Fi before card rename MAC=" + permMac);
                if (!wifi.setWifiEnabled(false)) throw new IllegalStateException("setWifiEnabled(false) rejected");
                long deadline = SystemClock.elapsedRealtime() + 15000;
                while (wifi.getWifiState() != WifiManager.WIFI_STATE_DISABLED && SystemClock.elapsedRealtime() < deadline) Thread.sleep(100);
                if (wifi.getWifiState() != WifiManager.WIFI_STATE_DISABLED) throw new IllegalStateException("Wi-Fi did not disable within 15 seconds");
            }
            IChannel channel = channels.channelFor("wifi");
            if (channel == null) throw new IllegalStateException("Wi-Fi channel unavailable");
            JSONObject args = new JSONObject(); args.put("device", permMac); args.put("persist", persist);
            Log.i(TAG, "Calling wifid select_device MAC=" + permMac);
            result = channel.call("select_device", args.toString());
            if (result == null || result.isEmpty()) throw new IllegalStateException("wifid returned an empty result");
            Log.i(TAG, "Wi-Fi switch succeeded MAC=" + permMac + " result=" + result);
        } catch (Exception e) {
            failure = e;
            Log.e(TAG, "Wi-Fi switch failed MAC=" + permMac, e);
        } finally {
            if (wasEnabled) {
                try {
                    Log.i(TAG, "Re-enabling Wi-Fi after card switch MAC=" + permMac);
                    if (!wifi.setWifiEnabled(true)) throw new IllegalStateException("setWifiEnabled(true) rejected");
                } catch (Exception e) {
                    Log.e(TAG, "Wi-Fi re-enable failed MAC=" + permMac, e);
                    if (failure == null) failure = e;
                }
            }
        }
        if (failure != null) throw new IllegalStateException("Wi-Fi switch failed for " + permMac + ": " + failure.getMessage(), failure);
        return result;
    }

    private void bindAndReconcile() {
        IChannel current = channels.channelFor("wifi");
        if (current == null) return;
        IChannel previous = subscribedChannel;
        if (previous != null && previous.asBinder().equals(current.asBinder())) return;
        if (previous != null) try { previous.unsubscribe("devices_changed", listener); } catch (Exception e) { Log.e(TAG, "Old Wi-Fi subscription removal failed", e); }
        try {
            current.subscribe("devices_changed", listener);
            subscribedChannel = current;
            Log.i(TAG, "Subscribed to wifi/devices_changed");
            reconcile();
        } catch (Exception e) { subscribedChannel = null; Log.e(TAG, "Wi-Fi channel subscription failed", e); }
    }

    private void reconcile() {
        try {
            IChannel channel = channels.channelFor("wifi"); if (channel == null) return;
            JSONObject state = new JSONObject(channel.call("get_state", "{}"));
            JSONObject snapshot = new JSONObject(channel.call("list_devices", "{}"));
            JSONArray devices = snapshot.optJSONArray("devices"); if (devices == null) throw new IllegalStateException("list_devices missing devices");
            String preferred = state.optString("selected", "");
            boolean preferredPresent = false, physicalAtWlan0 = false;
            List<String> candidates = new ArrayList<>();
            for (int i = 0; i < devices.length(); i++) {
                JSONObject d = devices.getJSONObject(i); if (d.optBoolean("simulated", false)) continue;
                String mac = d.optString("perm_mac", "");
                if (mac.isEmpty()) continue;
                if (mac.equals(preferred)) preferredPresent = true;
                if ("wlan0".equals(d.optString("interface"))) physicalAtWlan0 = true;
                candidates.add(mac);
            }
            if (preferredPresent && !preferred.equals(wlan0Mac(devices))) { Log.i(TAG, "Hotplug policy restoring preferred Wi-Fi MAC=" + preferred); switchNow(preferred, true); }
            else if (!physicalAtWlan0 && !candidates.isEmpty()) {
                Collections.sort(candidates, Comparator.naturalOrder());
                Log.i(TAG, "Hotplug policy selecting first physical Wi-Fi MAC=" + candidates.get(0));
                switchNow(candidates.get(0), false);
            }
        } catch (Exception e) { Log.e(TAG, "Wi-Fi hotplug reconciliation failed", e); }
    }
    private String wlan0Mac(JSONArray devices) throws Exception {
        for (int i = 0; i < devices.length(); i++) { JSONObject d = devices.getJSONObject(i); if ("wlan0".equals(d.optString("interface"))) return d.optString("perm_mac", ""); }
        return "";
    }
}
