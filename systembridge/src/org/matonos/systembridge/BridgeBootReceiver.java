package org.matonos.systembridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Starts the persistent bridge at boot and reapplies current-user input settings after switches. */
public final class BridgeBootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null) return;
        String action = intent.getAction();
        if (Intent.ACTION_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_USER_SWITCHED.equals(action)) {
            context.startService(new Intent(context, SystemBridgeService.class));
            if (Intent.ACTION_BOOT_COMPLETED.equals(action)) {
                PendingResult pending = goAsync();
                new Thread(() -> {
                    try { PreinstalledMusic.install(context); }
                    finally { pending.finish(); }
                }, "MatonPreinstalledMusic").start();
            }
        }
    }
}
