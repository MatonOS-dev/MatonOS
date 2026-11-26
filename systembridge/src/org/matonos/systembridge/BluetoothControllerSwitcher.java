package org.matonos.systembridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

import vendor.matonos.channel.IChannel;
import vendor.matonos.channel.IChannelListener;

/** Serializes Bluetooth card selection while the Bluetooth stack is off. */
final class BluetoothControllerSwitcher {
    private static final String TAG = "MatonSystemBridge";
    interface ChannelProvider { IChannel channelFor(String target); }

    private final Context context;
    private final BluetoothAdapter bluetooth;
    private final ChannelProvider channels;
    private final ScheduledExecutorService executor = Executors.newSingleThreadScheduledExecutor();
    private volatile IChannel subscribedChannel;
    // null until btd answers; true/false = a controller is/is not present.
    private volatile Boolean controllerPresent;
    private final BroadcastReceiver stateReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent intent) {
            if (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1) == BluetoothAdapter.STATE_TURNING_ON
                    && Boolean.FALSE.equals(controllerPresent)) {
                executor.execute(() -> syncNoController(false));
            }
        }
    };
    private final IChannelListener listener = new IChannelListener.Stub() {
        @Override public int getInterfaceVersion() { return IChannelListener.VERSION; }
        @Override public String getInterfaceHash() { return IChannelListener.HASH; }
        @Override public void onEvent(String topic, String json) {
            if ("devices_changed".equals(topic)) executor.execute(this::reconcileSafely);
        }
        private void reconcileSafely() { reconcile(); }
    };

    BluetoothControllerSwitcher(Context context, ChannelProvider provider) {
        BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        this.context = context;
        bluetooth = manager == null ? null : manager.getAdapter();
        channels = provider;
    }

    void start() {
        context.registerReceiver(stateReceiver, new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED),
                Context.RECEIVER_NOT_EXPORTED);
        executor.scheduleWithFixedDelay(this::bindAndReconcile, 0, 3, TimeUnit.SECONDS);
    }
    void stop() {
        IChannel channel = subscribedChannel;
        subscribedChannel = null;
        if (channel != null) try { channel.unsubscribe("devices_changed", listener); }
        catch (Exception e) { Log.e(TAG, "Bluetooth devices_changed unsubscribe failed", e); }
        try { context.unregisterReceiver(stateReceiver); }
        catch (IllegalArgumentException e) { Log.e(TAG, "Bluetooth state receiver was not registered", e); }
        executor.shutdownNow();
    }

    String switchTo(String permMac, boolean persist) {
        if (permMac == null || permMac.isEmpty()) throw new IllegalStateException("Bluetooth card MAC is required");
        Future<String> future = executor.submit(() -> switchNow(permMac, persist));
        try { return future.get(30, TimeUnit.SECONDS); }
        catch (TimeoutException e) { future.cancel(true); Log.e(TAG, "Bluetooth switch timed out for MAC=" + permMac, e); throw new IllegalStateException("Bluetooth switch timed out for " + permMac, e); }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); Log.e(TAG, "Bluetooth switch interrupted for MAC=" + permMac, e); throw new IllegalStateException("Bluetooth switch interrupted for " + permMac, e); }
        catch (ExecutionException e) { Throwable cause = e.getCause(); if (cause instanceof IllegalStateException) throw (IllegalStateException) cause; Log.e(TAG, "Bluetooth switch failed for MAC=" + permMac, cause); throw new IllegalStateException("Bluetooth switch failed for " + permMac, cause); }
    }

    // Platform-signed system apps are exempt from the public API toggle restriction.
    @SuppressWarnings("deprecation")
    private String switchNow(String permMac, boolean persist) {
        boolean wasEnabled = bluetooth != null && (bluetooth.getState() == BluetoothAdapter.STATE_ON
                || bluetooth.getState() == BluetoothAdapter.STATE_TURNING_ON);
        Log.i(TAG, "Bluetooth switch begin MAC=" + permMac + " persist=" + persist + " enabled=" + wasEnabled);
        Exception failure = null;
        String result = null;
        try {
            if (bluetooth == null) throw new IllegalStateException("BluetoothAdapter unavailable");
            if (bluetooth.getState() != BluetoothAdapter.STATE_OFF) {
                Log.i(TAG, "Disabling Bluetooth before controller selection MAC=" + permMac);
                if (!bluetooth.disable()) throw new IllegalStateException("BluetoothAdapter.disable() rejected");
                long deadline = SystemClock.elapsedRealtime() + 15000;
                while (bluetooth.getState() != BluetoothAdapter.STATE_OFF && SystemClock.elapsedRealtime() < deadline) Thread.sleep(100);
                if (bluetooth.getState() != BluetoothAdapter.STATE_OFF) throw new IllegalStateException("Bluetooth did not disable within 15 seconds");
            }
            IChannel channel = channels.channelFor("bluetooth");
            if (channel == null) throw new IllegalStateException("Bluetooth channel unavailable");
            JSONObject args = new JSONObject(); args.put("device", permMac); args.put("persist", persist);
            Log.i(TAG, "Calling btd select_device MAC=" + permMac);
            result = channel.call("select_device", args.toString());
            if (result == null || result.isEmpty()) throw new IllegalStateException("btd returned an empty result");
            Log.i(TAG, "Bluetooth switch succeeded MAC=" + permMac + " result=" + result);
        } catch (Exception e) {
            failure = e;
            Log.e(TAG, "Bluetooth switch failed MAC=" + permMac, e);
        } finally {
            if (wasEnabled) {
                try {
                    Log.i(TAG, "Re-enabling Bluetooth after card switch MAC=" + permMac);
                    if (!bluetooth.enable()) throw new IllegalStateException("BluetoothAdapter.enable() rejected");
                } catch (Exception e) {
                    Log.e(TAG, "Bluetooth re-enable failed MAC=" + permMac, e);
                    if (failure == null) failure = e;
                }
            }
        }
        if (failure != null) throw new IllegalStateException("Bluetooth switch failed for " + permMac + ": " + failure.getMessage(), failure);
        return result;
    }

    private void bindAndReconcile() {
        IChannel current = channels.channelFor("bluetooth");
        if (current == null) return;
        IChannel previous = subscribedChannel;
        if (previous != null && previous.asBinder().equals(current.asBinder())) return;
        if (previous != null) try { previous.unsubscribe("devices_changed", listener); } catch (Exception e) { Log.e(TAG, "Old Bluetooth subscription removal failed", e); }
        try {
            current.subscribe("devices_changed", listener);
            subscribedChannel = current;
            Log.i(TAG, "Subscribed to bluetooth/devices_changed");
            reconcile();
        } catch (Exception e) { subscribedChannel = null; Log.e(TAG, "Bluetooth channel subscription failed", e); }
    }

    // The stock stack aborts ("Can't start HAL") when enabled without a controller and the
    // framework restarts it five times; hide the QS toggle and keep a saved "on" from looping.
    @SuppressWarnings("deprecation")
    private void syncNoController(boolean present) {
        controllerPresent = present;
        try { QsBluetoothTile.sync(context, present); }
        catch (Exception e) { Log.e(TAG, "QS Bluetooth tile sync failed (present=" + present + ")", e); }
        if (present || bluetooth == null) return;
        int s = bluetooth.getState();
        if (s == BluetoothAdapter.STATE_ON || s == BluetoothAdapter.STATE_TURNING_ON) {
            Log.w(TAG, "Bluetooth enabled without a controller; disabling");
            if (!bluetooth.disable()) Log.e(TAG, "BluetoothAdapter.disable() rejected with no controller");
        }
    }

    private void reconcile() {
        try {
            IChannel channel = channels.channelFor("bluetooth"); if (channel == null) return;
            JSONObject state = new JSONObject(channel.call("get_state", "{}"));
            JSONObject snapshot = new JSONObject(channel.call("list_devices", "{}"));
            JSONArray devices = snapshot.optJSONArray("devices"); if (devices == null) throw new IllegalStateException("list_devices missing devices");
            syncNoController(devices.length() > 0);
            String preferred = state.optString("selected", "");
            boolean present = false, needsSwitch = false;
            for (int i = 0; i < devices.length(); i++) {
                JSONObject d = devices.getJSONObject(i);
                if (preferred.equals(d.optString("address")) && !preferred.isEmpty()) {
                    present = true;
                    if (!d.optBoolean("bound")) needsSwitch = true;
                } else if (d.optBoolean("bound") && "USB".equals(d.optString("bus"))) {
                    needsSwitch = true;
                }
            }
            if (present && needsSwitch) switchNow(preferred, true);
        } catch (Exception e) { Log.e(TAG, "Bluetooth hotplug reconciliation failed", e); }
    }
}
