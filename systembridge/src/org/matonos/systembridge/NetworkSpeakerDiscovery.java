/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systembridge;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.LinkProperties;
import android.net.NetworkRequest;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.ServiceManager;
import android.util.Log;
import java.net.InetAddress;
import java.net.Inet6Address;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import org.json.JSONObject;
import vendor.matonos.channel.IChannel;

/** NSD belongs to the existing bridge. All callbacks and Binder work use one worker. */
final class NetworkSpeakerDiscovery {
    private static final String TAG = "MatonNetworkAudio";
    private static final String[] TYPES = {
        "_raop._tcp", "_snapcast-jsonrpc._tcp", "_snapcast-ctrl._tcp"
    };
    private final NsdManager nsd;
    private final ConnectivityManager connectivity;
    private final HandlerThread thread = new HandlerThread("network-speakers");
    private Handler handler;
    private boolean stopped;
    private IChannel channel;
    private final Map<String, JSONObject> published = new HashMap<>();
    private final Map<String, Entry> entries = new HashMap<>();
    private final Map<String, NsdManager.DiscoveryListener> browsers = new HashMap<>();
    private final ConnectivityManager.NetworkCallback networks = new ConnectivityManager.NetworkCallback() {
        @Override public void onLost(Network network) {
            handler.post(() -> {
                for (Entry entry : new ArrayList<>(entries.values())) {
                    if (network.equals(entry.service.getNetwork())) forget(entry);
                }
                publish();
            });
        }
    };
    NetworkSpeakerDiscovery(Context context) {
        nsd = context.getSystemService(NsdManager.class);
        connectivity = context.getSystemService(ConnectivityManager.class);
    }
    void start() {
        thread.start();
        handler = new Handler(thread.getLooper());
        handler.post(() -> {
            try {
                connectivity.registerNetworkCallback(new NetworkRequest.Builder().clearCapabilities().build(),
                        networks, handler);
            } catch (RuntimeException e) { Log.e(TAG, "network observer registration failed", e); }
            tick.run();
        });
    }
    void stop() {
        handler.post(() -> {
            stopped = true;
            handler.removeCallbacksAndMessages(null);
            for (Entry entry : new ArrayList<>(entries.values())) forget(entry);
            for (NsdManager.DiscoveryListener browser : new ArrayList<>(browsers.values())) {
                try { nsd.stopServiceDiscovery(browser); }
                catch (RuntimeException e) { Log.e(TAG, "stop discovery failed", e); }
            }
            try { connectivity.unregisterNetworkCallback(networks); }
            catch (RuntimeException e) { Log.e(TAG, "stop network observer failed", e); }
            send("network_reset", new JSONObject());
            thread.quitSafely();
        });
    }
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (stopped) return;
            for (String type : TYPES) if (!browsers.containsKey(type)) browse(type);
            publish();
            handler.postDelayed(this, 5000);
        }
    };
    private void browse(String type) {
        NsdManager.DiscoveryListener listener = new NsdManager.DiscoveryListener() {
            @Override public void onDiscoveryStarted(String t) { }
            @Override public void onDiscoveryStopped(String t) { handler.post(() -> browsers.remove(type, this)); }
            @Override public void onStartDiscoveryFailed(String t, int code) {
                Log.e(TAG, "NSD start failed type=" + t + " code=" + code);
                handler.post(() -> browsers.remove(type, this));
            }
            @Override public void onStopDiscoveryFailed(String t, int code) {
                Log.e(TAG, "NSD stop failed type=" + t + " code=" + code);
            }
            @Override public void onServiceFound(NsdServiceInfo info) {
                handler.post(() -> {
                    if (stopped) return;
                    String id = type + "/" + info.getServiceName() + "/" + info.getNetwork();
                    if (entries.containsKey(id)) return;
                    Entry entry = new Entry(id, type, info);
                    entries.put(id, entry);
                    watch(entry);
                });
            }
            @Override public void onServiceLost(NsdServiceInfo info) {
                handler.post(() -> {
                    String id = type + "/" + info.getServiceName() + "/" + info.getNetwork();
                    Entry entry = entries.get(id);
                    if (entry != null) forget(entry);
                    publish();
                });
            }
        };
        browsers.put(type, listener);
        try { nsd.discoverServices(type, NsdManager.PROTOCOL_DNS_SD, listener); }
        catch (RuntimeException e) { browsers.remove(type); Log.e(TAG, "NSD browse failed type=" + type, e); }
    }
    private final class Entry implements NsdManager.ServiceInfoCallback {
        final String id, type;
        NsdServiceInfo service;
        JSONObject properties;
        boolean registered;
        Entry(String id, String type, NsdServiceInfo info) { this.id = id; this.type = type; service = info; }
        @Override public void onServiceInfoCallbackRegistrationFailed(int code) {
            Log.e(TAG, "NSD callback failed id=" + id + " code=" + code);
            registered = false;
            properties = null;
            publish();
            handler.postDelayed(() -> watch(this), 5000);
        }
        @Override public void onServiceUpdated(NsdServiceInfo info) {
            if (stopped || entries.get(id) != this) return;
            service = info;
            try { properties = map(this); }
            catch (Exception e) { properties = null; Log.e(TAG, "NSD mapping failed id=" + id, e); }
            
            publish();
        }
        @Override public void onServiceLost() { properties = null; publish(); }
        @Override public void onServiceInfoCallbackUnregistered() { registered = false; }
    }
    private void watch(Entry entry) {
        if (stopped || entries.get(entry.id) != entry || entry.registered) return;
        try {
            entry.registered = true;
            nsd.registerServiceInfoCallback(entry.service, handler::post, entry);
        } catch (RuntimeException e) {
            entry.registered = false;
            Log.e(TAG, "NSD watch failed id=" + entry.id, e);
            handler.postDelayed(() -> watch(entry), 5000);
        }
    }
    private void forget(Entry entry) {
        entries.remove(entry.id);
        try { if (entry.registered) nsd.unregisterServiceInfoCallback(entry); }
        catch (RuntimeException e) { Log.e(TAG, "NSD unwatch failed id=" + entry.id, e); }

    }
    private void sendRemove(String id) {
        try { send("network_remove", new JSONObject().put("id", id)); }
        catch (Exception e) { Log.e(TAG, "remove serialization failed id=" + id, e); }
    }
    private void send(String command, JSONObject args) {
        if (channel == null) return;
        try { channel.call(command, args.toString()); }
        catch (Exception e) { channel = null; Log.e(TAG, "audio channel command failed=" + command, e); }
    }
    private void publish() {
        IBinder binder = ServiceManager.checkService("vendor.matonos.channel.IChannel/audio");
        if (binder == null) { channel = null; return; }
        if (channel == null || !binder.equals(channel.asBinder())) {
            channel = IChannel.Stub.asInterface(binder);
            send("network_reset", new JSONObject());
        }
        Map<String, JSONObject> desired = new HashMap<>();
        for (Entry entry : entries.values()) if (entry.properties != null) {
            String id = entry.properties.optString("id");
            desired.put(id, entry.properties);
        }
        for (String old : published.keySet()) if (!desired.containsKey(old)) sendRemove(old);
        // Replay repairs restarts; duplicates across service types/networks share a sink id.
        for (JSONObject props : desired.values()) send("network_upsert", props);
        if (channel != null) { published.clear(); published.putAll(desired); }

    }
    private static boolean has(String value, String token) {
        for (String item : value.split(",")) if (item.equals(token)) return true;
        return false;
    }
    private static String choose(String value, String[] tokens, String[] values, String fallback) {
        for (int i = 0; i < tokens.length; i++) if (has(value, tokens[i])) return values[i];
        return fallback;
    }
    private JSONObject map(Entry entry) throws Exception {
        NsdServiceInfo info = entry.service;
        InetAddress address = null;
        // Match stock Snapcast's IPv4 preference; preserve IPv6 zone on RAOP addresses.
        for (InetAddress host : info.getHostAddresses()) {
            if (address == null || host.getAddress().length == 4) address = host;
        }
        if (address == null || info.getPort() <= 0) return null;
        boolean raop = entry.type.equals(TYPES[0]);
        if (!raop && address.getAddress().length != 4) return null;
        String prefix = raop ? "raop" : "snapcast";
        int ifindex = address instanceof Inet6Address ? ((Inet6Address)address).getScopeId() : 0;
        String ifname = null;
        LinkProperties links = info.getNetwork() == null ? null : connectivity.getLinkProperties(info.getNetwork());
        if (links != null && links.getInterfaceName() != null) {
            ifname = links.getInterfaceName();
            NetworkInterface iface = NetworkInterface.getByName(ifname);
            if (iface != null) ifindex = iface.getIndex();
        }
        String ip = address.getHostAddress();
        if (raop && ifindex > 0 && !ip.contains("%") && address.isLinkLocalAddress()) ip += "%" + ifindex;
        String hostname = info.getHostname();
        if (hostname == null) hostname = address.getHostAddress();
        else if (!hostname.endsWith(".local") && !hostname.endsWith(".local.")) hostname += ".local";
        JSONObject props = new JSONObject().put("id", prefix + "/" + (raop ? info.getServiceName() : address.getHostAddress() + ":" + info.getPort())).put("kind", prefix)
                .put(prefix + ".ip", ip).put(prefix + ".port", info.getPort())
                .put(prefix + ".name", info.getServiceName()).put(prefix + ".domain", "local")
                .put(prefix + ".hostname", hostname).put(prefix + ".ifindex", ifindex)
                .put("node.description", info.getServiceName());
        if (!raop && ifname != null) props.put("snapcast.ifname", ifname).put("local.ifname", ifname);
        // Snapcast uses no TXT properties. RAOP mapping follows the pinned discover module.
        if (raop) for (Map.Entry<String, byte[]> attr : info.getAttributes().entrySet()) {
            if (attr.getValue() == null) continue;
            String value = new String(attr.getValue(), StandardCharsets.UTF_8);
            String key;
            switch (attr.getKey()) {
                case "device": key = "raop.device"; break;
                case "tp": key = "raop.transport"; value = choose(value,
                        new String[]{"UDP", "TCP"}, new String[]{"udp", "tcp"}, value); break;
                case "et": key = "raop.encryption.type"; value = choose(value,
                        new String[]{"5", "4", "1"}, new String[]{"fp_sap25", "auth_setup", "RSA"}, "none"); break;
                case "cn": key = "raop.audio.codec"; value = choose(value,
                        new String[]{"0", "1", "2", "3"}, new String[]{"PCM", "ALAC", "AAC", "AAC-ELD"}, "unknown"); break;
                case "ch": key = "audio.channels"; break;
                case "ss": key = "audio.format"; value = choose(value,
                        new String[]{"16", "24", "32"}, new String[]{"S16", "S24", "S32"}, "UNKNOWN"); break;
                case "sr": key = "audio.rate"; break;
                case "am": key = "device.model"; break;
                default: continue;
            }
            props.put(key, value);
        }
        return props;
    }
}
