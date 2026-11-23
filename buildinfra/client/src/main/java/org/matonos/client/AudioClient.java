package org.matonos.client;

/** Typed access to PipeWire device events through the existing bridge channel. */
public final class AudioClient {
    public interface DeviceListener { void onDevicesEvent(String jsonEvent); }

    private static final String TARGET = "audio";
    private static final String TOPIC = "devices";
    private final MatonosClient client;
    private final java.util.Map<DeviceListener, MatonosClient.EventListener> listeners =
            new java.util.concurrent.ConcurrentHashMap<>();

    public AudioClient(MatonosClient client) { this.client = client; }

    public MatonosClient.Result<String> devices() {
        return client.call(TARGET, "devices", "{}");
    }

    public MatonosClient.Result<Void> subscribe(DeviceListener listener) {
        MatonosClient.EventListener callback = (target, topic, event) -> {
            if (listener != null) listener.onDevicesEvent(event);
        };
        listeners.put(listener, callback);
        return client.subscribe(TARGET, TOPIC, callback);
    }

    public MatonosClient.Result<Void> unsubscribe(DeviceListener listener) {
        MatonosClient.EventListener callback = listeners.remove(listener);
        return callback == null ? MatonosClient.Result.value(null)
                : client.unsubscribe(TARGET, TOPIC, callback);
    }
}
