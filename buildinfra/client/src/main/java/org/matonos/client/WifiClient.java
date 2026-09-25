package org.matonos.client;

import org.json.JSONException;
import org.json.JSONObject;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** Typed wrapper for wifid's adapter selection and state event. */
public final class WifiClient {
    public interface StateListener { void onState(State state); }
    public static final class State {
        public final boolean present;
        public final String selected;
        public final boolean testApReady;
        State(JSONObject json) throws JSONException {
            present = json.getBoolean("present");
            selected = json.getString("selected");
            testApReady = json.getBoolean("testApReady");
        }
    }

    private final MatonosClient client;
    private final Map<StateListener, MatonosClient.EventListener> listeners = new ConcurrentHashMap<>();
    public WifiClient(MatonosClient client) { this.client = client; }
    public MatonosClient.Result<State> getState() {
        MatonosClient.Result<String> response = client.call("wifi", "get_state", "{}");
        if (!response.available) return MatonosClient.Result.unavailable(response.reason);
        try { return MatonosClient.Result.value(new State(new JSONObject(response.value))); }
        catch (JSONException e) { return MatonosClient.Result.unavailable("INVALID_WIFI_STATE"); }
    }
    public MatonosClient.Result<String> listDevices() {
        return client.call("wifi", "list_devices", "{}");
    }
    public MatonosClient.Result<Boolean> selectDevice(String device) {
        try {
            JSONObject args = new JSONObject(); args.put("device", device);
            MatonosClient.Result<String> response = client.call("wifi", "select_device", args.toString());
            if (!response.available) return MatonosClient.Result.unavailable(response.reason);
            JSONObject result = new JSONObject(response.value);
            return MatonosClient.Result.value(device.equals(result.optString("selected")));
        } catch (JSONException e) { return MatonosClient.Result.unavailable("INVALID_WIFI_RESULT"); }
    }
    public MatonosClient.Result<Void> subscribe(StateListener listener) {
        MatonosClient.EventListener callback = (target, topic, event) -> {
            try { listener.onState(new State(new JSONObject(event))); }
            catch (JSONException ignored) { }
        };
        listeners.put(listener, callback);
        return client.subscribe("wifi", "state", callback);
    }
    public MatonosClient.Result<Void> unsubscribe(StateListener listener) {
        MatonosClient.EventListener callback = listeners.remove(listener);
        return callback == null ? MatonosClient.Result.value(null) : client.unsubscribe("wifi", "state", callback);
    }
}
