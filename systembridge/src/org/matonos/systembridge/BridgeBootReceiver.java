package org.matonos.systembridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.SystemProperties;
import android.util.Log;

/** Starts the persistent bridge at boot and reapplies current-user input settings after switches. */
public final class BridgeBootReceiver extends BroadcastReceiver {
    private static final String TAG = "MatonGraphicsWarning";
    private static final String GRAPHICS_CHANNEL = "graphics";
    private static final int GRAPHICS_WARNING_ID = 1;

    @Override public void onReceive(Context context, Intent intent) {
        if (intent == null) return;
        String action = intent.getAction();
        if (Intent.ACTION_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_USER_SWITCHED.equals(action)) {
            context.startService(new Intent(context, SystemBridgeService.class));
            if (Intent.ACTION_BOOT_COMPLETED.equals(action)) {
                postSoftwareGraphicsWarning(context);
                PendingResult pending = goAsync();
                new Thread(() -> {
                    try { PreinstalledMusic.install(context); }
                    finally { pending.finish(); }
                }, "MatonPreinstalledMusic").start();
            }
        }
    }

    private static void postSoftwareGraphicsWarning(Context context) {
        try {
            if (!"swrast".equals(SystemProperties.get("vendor.maton.graphics.vulkan"))) {
                return;
            }
            String reason = SystemProperties.get("vendor.maton.graphics.vulkan_reason");
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            if (manager == null) {
                Log.e(TAG, "Cannot post software Vulkan warning: NotificationManager unavailable");
                return;
            }
            manager.createNotificationChannel(new NotificationChannel(
                    GRAPHICS_CHANNEL, "Graphics", NotificationManager.IMPORTANCE_DEFAULT));
            String message = "No hardware graphics driver — using software rendering. "
                    + "Apps and games will be slow.";
            if (!reason.isEmpty()) message += " " + reason;
            Notification notification = new Notification.Builder(context, GRAPHICS_CHANNEL)
                    .setSmallIcon(android.R.drawable.stat_sys_warning)
                    .setContentTitle("Software graphics rendering")
                    .setContentText(message)
                    .setStyle(new Notification.BigTextStyle().bigText(message))
                    .setAutoCancel(false)
                    .build();
            manager.notify(GRAPHICS_WARNING_ID, notification);
        } catch (RuntimeException e) {
            Log.e(TAG, "Failed to post software Vulkan warning at boot", e);
        }
    }
}
