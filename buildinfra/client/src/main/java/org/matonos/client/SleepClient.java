package org.matonos.client;

import android.os.RemoteException;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** Typed wrapper for matonos-sleepd's state and state event. */
public final class SleepClient {
    public interface StateListener { void onState(State state); }
    public static final class State {
        public final int idleTimeoutSeconds;
        public final boolean suspendSupported;
        public final boolean sleeping;
        State(JSONObject json) throws JSONException {
            idleTimeoutSeconds = json.getInt("idleTimeoutSeconds");
            suspendSupported = json.getBoolean("suspendSupported");
            sleeping = json.getBoolean("sleeping");
        }
        @Override public String toString() {
            return "Idle timeout: " + (idleTimeoutSeconds == 0 ? "Never" : idleTimeoutSeconds + " seconds") +
                    "\nSuspend supported: " + suspendSupported + "\nSleeping: " + sleeping;
        }
    }

    private final MatonosClient client;
    private final Map<StateListener, MatonosClient.EventListener> listeners = new ConcurrentHashMap<>();
    public SleepClient(MatonosClient client) { this.client = client; }
    public MatonosClient.Result<State> getState() {
        MatonosClient.Result<String> response = client.call("sleep", "get_state", "{}");
        if (!response.available) return MatonosClient.Result.unavailable(response.reason);
        try { return MatonosClient.Result.value(new State(new JSONObject(response.value))); }
        catch (JSONException e) { return MatonosClient.Result.unavailable("INVALID_SLEEP_STATE"); }
    }
    public MatonosClient.Result<Void> subscribe(StateListener listener) {
        MatonosClient.EventListener callback = (target, topic, event) -> {
            try { listener.onState(new State(new JSONObject(event))); }
            catch (JSONException ignored) { }
        };
        listeners.put(listener, callback);
        return client.subscribe("sleep", "state", callback);
    }
    public MatonosClient.Result<Void> unsubscribe(StateListener listener) {
        MatonosClient.EventListener callback = listeners.remove(listener);
        return callback == null ? MatonosClient.Result.value(null) : client.unsubscribe("sleep", "state", callback);
    }
}
