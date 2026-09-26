package org.matonos.shelf;

import android.content.Context;

import org.matonos.systembridge.ISystemBridge;
import org.matonos.client.BridgeMode;
import org.matonos.client.MatonOS;
import org.matonos.client.MatonosClient;

/** Typed access to the small set of privileged operations owned by System Bridge. */
final class ShellBridge implements AutoCloseable {
    interface Listener { void onBridgeChanged(ISystemBridge bridge); }

    private final Listener listener;
    private final MatonosClient client;
    private final MatonosClient.AvailabilityListener availability;

    ShellBridge(Context context, Listener listener) {
        Context appContext = context.getApplicationContext();
        this.listener = listener;
        this.client = MatonOS.init(appContext, BridgeMode.OPTIONAL);
        this.availability = this::onAvailabilityChanged;
    }

    private void onAvailabilityChanged(boolean ready, String reason) {
        listener.onBridgeChanged(ready ? client.getBridgeInterface() : null);
    }

    void connect() {
        client.addAvailabilityListener(availability);
    }

    ISystemBridge get() { return client.isAvailable() ? client.getBridgeInterface() : null; }

    boolean navigate(String action) { return navigate(action, false); }

    boolean navigate(String action, boolean longPress) {
        String method = switch (action) {
            case "back" -> "navigateBack";
            case "home" -> "navigateHome";
            case "recents" -> "navigateRecents";
            default -> "";
        };
        if (method.isEmpty()) return false;
        try {
            Object result = "back".equals(action)
                    ? MatonosClient.class.getMethod(method, boolean.class).invoke(client, longPress)
                    : MatonosClient.class.getMethod(method).invoke(client);
            java.lang.reflect.Field available = result.getClass().getField("available");
            java.lang.reflect.Field value = result.getClass().getField("value");
            return available.getBoolean(result) && Boolean.TRUE.equals(value.get(result));
        } catch (ReflectiveOperationException | RuntimeException error) {
            android.util.Log.w("MatonOSShelf", "Navigation capability unavailable: " + action, error);
            return false;
        }
    }

    @Override public void close() {
        client.removeAvailabilityListener(availability);
        client.close();
    }

}
