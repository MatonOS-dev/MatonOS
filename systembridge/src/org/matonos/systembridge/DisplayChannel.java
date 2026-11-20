package org.matonos.systembridge;

import android.content.Context;
import android.graphics.PointF;
import android.graphics.RectF;
import android.hardware.display.DisplayManager;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.DisplayTopology;
import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.UserHandle;
import android.provider.Settings;
import android.util.Log;
import android.util.SparseArray;
import android.view.Display;
import android.view.Surface;
import android.view.IWindowManager;
import android.view.WindowManagerGlobal;

import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.Consumer;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import vendor.matonos.channel.IChannel;
import vendor.matonos.channel.IChannelListener;

/**
 * The "display" target: monitor list, on/off, mode, scale, rotation, mirroring and arrangement
 * for MatonOS Settings. Runs inside the bridge because it needs the framework display APIs.
 */
final class DisplayChannel extends IChannel.Stub {
    static final String TARGET = "display";
    private static final String TAG = "MatonDisplayChannel";
    private static final String TOPIC_CHANGED = "changed";

    private final Context context;
    private final DisplayManager displayManager;
    private final IWindowManager windowManager = WindowManagerGlobal.getWindowManagerService();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final CopyOnWriteArrayList<IChannelListener> listeners = new CopyOnWriteArrayList<>();
    private boolean watching;

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int displayId) { notifyChanged(); }
        @Override public void onDisplayRemoved(int displayId) { notifyChanged(); }
        @Override public void onDisplayChanged(int displayId) { notifyChanged(); }
    };
    private final Consumer<DisplayTopology> topologyListener = topology -> notifyChanged();

    DisplayChannel(Context context) {
        this.context = context;
        this.displayManager = context.getSystemService(DisplayManager.class);
    }

    @Override public int getInterfaceVersion() { return IChannel.VERSION; }
    @Override public String getInterfaceHash() { return IChannel.HASH; }

    @Override public String call(String command, String jsonArgs) {
        long token = Binder.clearCallingIdentity();
        try {
            JSONObject args = new JSONObject(jsonArgs == null ? "{}" : jsonArgs);
            switch (command) {
                case "get_state": return state().toString();
                case "set_enabled": return setEnabled(args.getInt("id"), args.getBoolean("enabled"));
                case "set_mode": return setMode(args.getInt("id"), args.getInt("width"),
                        args.getInt("height"), (float) args.getDouble("refresh"));
                case "set_scale": return setScale(args.getInt("id"), args.getInt("percent"));
                case "set_rotation": return setRotation(args.getInt("id"), args.getInt("rotation"));
                case "set_mirror": return setMirror(args.getBoolean("mirror"));
                case "set_position": return setPosition(args.getInt("id"), args.getString("side"));
                default: throw new IllegalArgumentException("unknown display command: " + command);
            }
        } catch (JSONException e) {
            throw new IllegalArgumentException("invalid display arguments for " + command, e);
        } finally {
            Binder.restoreCallingIdentity(token);
        }
    }

    @Override public void subscribe(String topic, IChannelListener listener) {
        if (!TOPIC_CHANGED.equals(topic)) throw new IllegalArgumentException("unknown display topic: " + topic);
        listeners.addIfAbsent(listener);
        handler.post(this::updateWatching);
    }

    @Override public void unsubscribe(String topic, IChannelListener listener) {
        listeners.remove(listener);
        handler.post(this::updateWatching);
    }

    @Override public String stageUpdate(String[] names, ParcelFileDescriptor[] files) {
        throw new UnsupportedOperationException("display target has no updates");
    }
    @Override public String finalizeUpdate() {
        throw new UnsupportedOperationException("display target has no updates");
    }
    @Override public String getUpdateStatus() {
        throw new UnsupportedOperationException("display target has no updates");
    }

    private void updateWatching() {
        boolean wanted = !listeners.isEmpty();
        if (wanted == watching) return;
        long token = Binder.clearCallingIdentity();
        try {
            if (wanted) {
                displayManager.registerDisplayListener(displayListener, handler);
                displayManager.registerTopologyListener(context.getMainExecutor(), topologyListener);
            } else {
                displayManager.unregisterDisplayListener(displayListener);
                displayManager.unregisterTopologyListener(topologyListener);
            }
            watching = wanted;
        } finally {
            Binder.restoreCallingIdentity(token);
        }
    }

    private void notifyChanged() {
        for (IChannelListener listener : listeners) {
            try {
                listener.onEvent(TOPIC_CHANGED, "{}");
            } catch (RemoteException e) {
                Log.w(TAG, "Display listener died; dropping it", e);
                listeners.remove(listener);
            }
        }
        if (listeners.isEmpty()) updateWatching();
    }

    private JSONObject state() throws JSONException {
        Set<Integer> enabled = new HashSet<>();
        for (Display display : displayManager.getDisplays()) enabled.add(display.getDisplayId());
        DisplayTopology topology = displayManager.getDisplayTopology();
        SparseArray<RectF> bounds = topology == null ? new SparseArray<>() : topology.getAbsoluteBounds();
        int primary = topology == null ? Display.DEFAULT_DISPLAY : topology.getPrimaryDisplayId();

        JSONArray displays = new JSONArray();
        for (Display display : displayManager.getDisplays(
                DisplayManager.DISPLAY_CATEGORY_ALL_INCLUDING_DISABLED)) {
            int type = display.getType();
            if (type != Display.TYPE_INTERNAL && type != Display.TYPE_EXTERNAL) continue;
            int id = display.getDisplayId();
            JSONObject entry = new JSONObject();
            entry.put("id", id);
            entry.put("name", display.getName());
            entry.put("external", type == Display.TYPE_EXTERNAL);
            entry.put("enabled", enabled.contains(id));
            entry.put("primary", id == primary);
            Display.Mode mode = display.getMode();
            entry.put("width", mode.getPhysicalWidth());
            entry.put("height", mode.getPhysicalHeight());
            entry.put("refresh", mode.getRefreshRate());
            entry.put("modes", modes(display));
            entry.put("rotation", display.getRotation());
            entry.put("scale", scalePercent(id));
            RectF rect = bounds.get(id);
            if (rect != null) {
                JSONObject place = new JSONObject();
                place.put("left", rect.left); place.put("top", rect.top);
                place.put("width", rect.width()); place.put("height", rect.height());
                entry.put("bounds", place);
            }
            displays.put(entry);
        }
        JSONObject state = new JSONObject();
        state.put("mirror", Settings.Secure.getInt(context.getContentResolver(),
                Settings.Secure.MIRROR_BUILT_IN_DISPLAY, 0) == 1);
        state.put("displays", displays);
        return state;
    }

    private static JSONArray modes(Display display) throws JSONException {
        // One entry per size/refresh pair, largest first, as the panel reports them.
        Map<String, Display.Mode> unique = new LinkedHashMap<>();
        for (Display.Mode mode : display.getSupportedModes()) {
            String key = mode.getPhysicalWidth() + "x" + mode.getPhysicalHeight() + "@"
                    + Math.round(mode.getRefreshRate() * 100);
            unique.putIfAbsent(key, mode);
        }
        JSONArray list = new JSONArray();
        unique.values().stream()
                .sorted((a, b) -> {
                    long areaA = (long) a.getPhysicalWidth() * a.getPhysicalHeight();
                    long areaB = (long) b.getPhysicalWidth() * b.getPhysicalHeight();
                    if (areaA != areaB) return Long.compare(areaB, areaA);
                    return Float.compare(b.getRefreshRate(), a.getRefreshRate());
                })
                .forEach(mode -> {
                    try {
                        JSONObject entry = new JSONObject();
                        entry.put("width", mode.getPhysicalWidth());
                        entry.put("height", mode.getPhysicalHeight());
                        entry.put("refresh", mode.getRefreshRate());
                        list.put(entry);
                    } catch (JSONException e) {
                        throw new IllegalStateException(e);
                    }
                });
        return list;
    }

    private int scalePercent(int displayId) {
        try {
            int initial = windowManager.getInitialDisplayDensity(displayId);
            int base = windowManager.getBaseDisplayDensity(displayId);
            if (initial <= 0 || base <= 0) return 100;
            return Math.round(base * 100f / initial);
        } catch (RemoteException e) {
            throw new IllegalStateException("window manager unavailable", e);
        }
    }

    private Display require(int displayId) {
        for (Display display : displayManager.getDisplays(
                DisplayManager.DISPLAY_CATEGORY_ALL_INCLUDING_DISABLED)) {
            if (display.getDisplayId() == displayId) return display;
        }
        throw new IllegalArgumentException("no display " + displayId);
    }

    private String setEnabled(int displayId, boolean enable) {
        Display display = require(displayId);
        if (display.getType() != Display.TYPE_EXTERNAL)
            throw new IllegalArgumentException("only external displays can be turned off");
        if (enable) displayManager.enableConnectedDisplay(displayId);
        else displayManager.disableConnectedDisplay(displayId);
        Log.i(TAG, (enable ? "Enabled" : "Disabled") + " display " + displayId);
        return enable ? "\"Display turned on\"" : "\"Display turned off\"";
    }

    private String setMode(int displayId, int width, int height, float refresh) {
        Display display = require(displayId);
        Display.Mode match = null;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == width && mode.getPhysicalHeight() == height
                    && Math.abs(mode.getRefreshRate() - refresh) < 0.01f) {
                match = mode;
                break;
            }
        }
        if (match == null) throw new IllegalArgumentException("display " + displayId
                + " has no mode " + width + "x" + height + "@" + refresh);
        DisplayManagerGlobal.getInstance().setUserPreferredDisplayMode(displayId, match, true);
        Log.i(TAG, "Display " + displayId + " mode " + width + "x" + height + "@" + refresh);
        return "\"Resolution applied\"";
    }

    private String setScale(int displayId, int percent) {
        require(displayId);
        if (percent < 50 || percent > 300)
            throw new IllegalArgumentException("scale must be between 50 and 300 percent");
        try {
            int initial = windowManager.getInitialDisplayDensity(displayId);
            if (percent == 100) {
                windowManager.clearForcedDisplayDensityForUser(displayId, UserHandle.USER_CURRENT);
            } else {
                windowManager.setForcedDisplayDensityForUser(displayId,
                        Math.round(initial * percent / 100f), UserHandle.USER_CURRENT);
            }
        } catch (RemoteException e) {
            throw new IllegalStateException("window manager unavailable", e);
        }
        Log.i(TAG, "Display " + displayId + " scale " + percent + "%");
        return "\"Scale applied\"";
    }

    private String setRotation(int displayId, int rotation) {
        require(displayId);
        if (rotation < Surface.ROTATION_0 || rotation > Surface.ROTATION_270)
            throw new IllegalArgumentException("rotation must be 0-3");
        try {
            windowManager.freezeDisplayRotation(displayId, rotation, "MatonOS Settings");
        } catch (RemoteException e) {
            throw new IllegalStateException("window manager unavailable", e);
        }
        Log.i(TAG, "Display " + displayId + " rotation " + rotation);
        return "\"Rotation applied\"";
    }

    private String setMirror(boolean mirror) {
        Settings.Secure.putInt(context.getContentResolver(),
                Settings.Secure.MIRROR_BUILT_IN_DISPLAY, mirror ? 1 : 0);
        Log.i(TAG, "Mirror built-in display: " + mirror);
        return mirror ? "\"Mirroring displays\"" : "\"Extending desktop\"";
    }

    private String setPosition(int displayId, String side) {
        DisplayTopology topology = displayManager.getDisplayTopology();
        if (topology == null) throw new IllegalStateException("no display topology");
        topology = topology.copy();
        int primary = topology.getPrimaryDisplayId();
        if (displayId == primary) throw new IllegalArgumentException("the main display stays in place");
        SparseArray<RectF> bounds = topology.getAbsoluteBounds();
        RectF anchor = bounds.get(primary);
        RectF moved = bounds.get(displayId);
        if (anchor == null || moved == null)
            throw new IllegalArgumentException("display " + displayId + " is not arranged");
        PointF target;
        switch (side) {
            case "left": target = new PointF(anchor.left - moved.width(), anchor.top); break;
            case "right": target = new PointF(anchor.right, anchor.top); break;
            case "above": target = new PointF(anchor.left, anchor.top - moved.height()); break;
            case "below": target = new PointF(anchor.left, anchor.bottom); break;
            default: throw new IllegalArgumentException("side must be left, right, above or below");
        }
        Map<Integer, PointF> positions = new HashMap<>();
        for (int i = 0; i < bounds.size(); i++) {
            RectF rect = bounds.valueAt(i);
            positions.put(bounds.keyAt(i), new PointF(rect.left, rect.top));
        }
        positions.put(displayId, target);
        topology.rearrange(positions);
        displayManager.setDisplayTopology(topology);
        Log.i(TAG, "Display " + displayId + " placed " + side + " of display " + primary);
        return "\"Arrangement applied\"";
    }
}
