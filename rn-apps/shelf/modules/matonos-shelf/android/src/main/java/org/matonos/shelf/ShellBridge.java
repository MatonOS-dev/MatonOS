package org.matonos.shelf;

import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.WindowManager;

import org.matonos.systembridge.ISystemBridge;
import org.matonos.client.BridgeMode;
import org.matonos.client.MatonOS;
import org.matonos.client.MatonosClient;

/** Typed access to the small set of privileged operations owned by System Bridge. */
final class ShellBridge implements AutoCloseable {
    interface Listener { void onBridgeChanged(boolean available); }

    private final Listener listener;
    private final MatonosClient client;
    private final MatonosClient.AvailabilityListener availability;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    ShellBridge(Context context, Listener listener) {
        Context appContext = context.getApplicationContext();
        this.listener = listener;
        this.client = MatonOS.init(appContext, BridgeMode.OPTIONAL);
        this.availability = this::onAvailabilityChanged;
    }

    private void onAvailabilityChanged(boolean ready, String reason) {
        // Availability checks complete on matonos-bridge-check. WindowManager and
        // ShelfService lifecycle work must always run on the main thread.
        if (!ready && "BRIDGE_CONNECTING".equals(reason)) return;
        mainHandler.post(() -> listener.onBridgeChanged(ready));
    }

    void connect() {
        client.addAvailabilityListener(availability);
    }

    ISystemBridge get() { return client.isAvailable() ? client.getBridgeInterface() : null; }

    MatonosClient.Result<Boolean> ensureShellOverlayAccess() {
        return client.ensureShellOverlayAccess();
    }

    MatonosClient.Result<Bundle> prepareShellOverlay(WindowManager.LayoutParams params) {
        return client.prepareShellOverlay(params);
    }

    boolean navigate(String action) { return navigate(action, false); }

    boolean navigate(String action, boolean longPress) {
        MatonosClient.Result<Boolean> result = switch (action) {
            case "back" -> client.navigateBack(longPress);
            case "home" -> client.navigateHome();
            case "recents" -> client.navigateRecents();
            default -> MatonosClient.Result.unavailable("UNKNOWN_NAVIGATION_ACTION");
        };
        if (!result.available) android.util.Log.w("MatonOSShelf", "Navigation capability unavailable: " + action + ": " + result.reason);
        return result.available && Boolean.TRUE.equals(result.value);
    }

    @Override public void close() {
        client.removeAvailabilityListener(availability);
        client.close();
    }

}
