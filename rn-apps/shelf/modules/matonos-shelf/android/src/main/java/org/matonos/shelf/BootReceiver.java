package org.matonos.shelf;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Starts the persistent shelf at boot and relays the shell's foreground state. */
public final class BootReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        Intent start = new Intent(context, ShelfService.class);
        if (ShelfService.ACTION_HOME_VISIBLE.equals(intent.getAction())) {
            start.setAction(ShelfService.ACTION_HOME_VISIBLE)
                    .putExtra(ShelfService.EXTRA_HOME_VISIBLE,
                            intent.getBooleanExtra(ShelfService.EXTRA_HOME_VISIBLE, false));
        } else if (ShelfService.ACTION_TOGGLE_PIN.equals(intent.getAction())) {
            start.setAction(ShelfService.ACTION_TOGGLE_PIN)
                    .putExtra("packageName", intent.getStringExtra("packageName"));
        }
        try { context.startService(start); }
        catch (RuntimeException error) {
            android.util.Log.e("MatonOSShelf", "Could not start shelf service", error);
        }
    }
}
