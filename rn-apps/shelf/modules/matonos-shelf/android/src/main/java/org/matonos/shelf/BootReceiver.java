package org.matonos.shelf;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Relays shell state changes; system-bridge binding owns provider startup at boot. */
public final class BootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null || Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())
                || Intent.ACTION_USER_UNLOCKED.equals(intent.getAction())) return;
        // The platform bridge binds the provider only after the user selects it.
        // These state broadcasts are transient and are safe to drop while it is
        // not bound; starting a background service from a receiver is forbidden.
        ShelfService.handleBroadcast(intent);
    }
}
