package org.matonos.shelf;

import android.app.Notification;
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.Icon;
import android.os.Bundle;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Notification source for the Shelf's own notification area (Windows-style tray).
 * Needs notification-listener access (granted automatically to the built-in Shelf by
 * the system bridge; for development: `cmd notification allow_listener
 * org.matonos.shelf/org.matonos.shelf.ShelfNotificationListener`).
 */
public final class ShelfNotificationListener extends NotificationListenerService {
    private static final String TAG = "ShelfNotifications";
    private static volatile ShelfNotificationListener active;

    static boolean isConnected() { return active != null; }

    @Override public void onListenerConnected() {
        active = this;
        MatonShelfExpoModule.notifyNotificationsChanged("connected", null);
    }

    @Override public void onListenerDisconnected() {
        active = null;
        MatonShelfExpoModule.notifyNotificationsChanged("disconnected", null);
    }

    @Override public void onNotificationPosted(StatusBarNotification sbn) {
        MatonShelfExpoModule.notifyNotificationsChanged("posted", sbn.getKey());
    }

    @Override public void onNotificationRemoved(StatusBarNotification sbn) {
        MatonShelfExpoModule.notifyNotificationsChanged("removed", sbn.getKey());
    }

    /** Current notifications, newest first; empty when access is not granted. */
    static List<Map<String, Object>> snapshot(Context context) {
        ShelfNotificationListener listener = active;
        List<Map<String, Object>> out = new ArrayList<>();
        if (listener == null) return out;
        StatusBarNotification[] all;
        try { all = listener.getActiveNotifications(); }
        catch (RuntimeException failure) { Log.w(TAG, "getActiveNotifications failed", failure); return out; }
        if (all == null) return out;
        java.util.Arrays.sort(all, (a, b) -> Long.compare(b.getPostTime(), a.getPostTime()));
        Ranking ranking = new Ranking();
        RankingMap rankings = null;
        try { rankings = listener.getCurrentRanking(); } catch (RuntimeException ignored) { }
        for (StatusBarNotification sbn : all) {
            Map<String, Object> map = toMap(context, sbn);
            if (rankings != null && rankings.getRanking(sbn.getKey(), ranking)) {
                map.put("importance", ranking.getImportance());
                // Silent = the shade's "Silent" section (low/min importance, no alert).
                map.put("silent", ranking.getImportance() <= android.app.NotificationManager.IMPORTANCE_LOW);
            } else {
                map.put("importance", -1);
                map.put("silent", false);
            }
            out.add(map);
        }
        return out;
    }

    private static Map<String, Object> toMap(Context context, StatusBarNotification sbn) {
        Notification n = sbn.getNotification();
        Bundle extras = n.extras;
        Map<String, Object> map = new HashMap<>();
        map.put("key", sbn.getKey());
        map.put("packageName", sbn.getPackageName());
        map.put("appLabel", appLabel(context, sbn.getPackageName()));
        map.put("title", text(extras, Notification.EXTRA_TITLE));
        map.put("text", text(extras, Notification.EXTRA_TEXT));
        map.put("subText", text(extras, Notification.EXTRA_SUB_TEXT));
        map.put("postTime", (double) sbn.getPostTime());
        map.put("ongoing", sbn.isOngoing());
        map.put("clearable", sbn.isClearable());
        map.put("groupKey", sbn.getGroupKey());
        map.put("isGroupSummary", (n.flags & Notification.FLAG_GROUP_SUMMARY) != 0);
        map.put("category", n.category == null ? "" : n.category);
        map.put("iconUri", iconUri(context, sbn, false));
        map.put("smallIconUri", iconUri(context, sbn, true));
        map.put("color", n.color);  // app accent (ARGB int), 0 = none
        map.put("bigText", text(extras, Notification.EXTRA_BIG_TEXT));
        List<String> lines = new ArrayList<>();
        CharSequence[] textLines = extras == null ? null : extras.getCharSequenceArray(Notification.EXTRA_TEXT_LINES);
        if (textLines != null) for (CharSequence line : textLines) lines.add(line == null ? "" : line.toString());
        map.put("lines", lines);
        List<Map<String, Object>> actions = new ArrayList<>();
        if (n.actions != null) for (int i = 0; i < n.actions.length; i++) {
            Notification.Action a = n.actions[i];
            Map<String, Object> action = new HashMap<>();
            action.put("index", i);
            action.put("title", a.title == null ? "" : a.title.toString());
            action.put("reply", a.getRemoteInputs() != null && a.getRemoteInputs().length > 0);
            actions.add(action);
        }
        map.put("actions", actions);
        return map;
    }

    private static String text(Bundle extras, String key) {
        CharSequence value = extras == null ? null : extras.getCharSequence(key);
        return value == null ? "" : value.toString();
    }

    private static String appLabel(Context context, String pkg) {
        try {
            PackageManager pm = context.getPackageManager();
            return pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString();
        } catch (PackageManager.NameNotFoundException e) {
            return pkg;
        }
    }

    /** small=true: the (monochrome) small icon; else large icon if any, else small. Cached PNG. */
    private static String iconUri(Context context, StatusBarNotification sbn, boolean small) {
        try {
            Notification n = sbn.getNotification();
            Icon icon = small || n.getLargeIcon() == null ? n.getSmallIcon() : n.getLargeIcon();
            if (icon == null) return "";
            Context pkgContext = context.createPackageContext(sbn.getPackageName(), 0);
            Drawable drawable = icon.loadDrawable(pkgContext);
            if (drawable == null) return "";
            int size = Math.round(48 * context.getResources().getDisplayMetrics().density);
            Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            drawable.setBounds(0, 0, size, size);
            drawable.draw(new Canvas(bitmap));
            File dir = new File(context.getCacheDir(), "notification-icons");
            if (!dir.isDirectory() && !dir.mkdirs()) return "";
            File file = new File(dir, Integer.toHexString(sbn.getKey().hashCode()) + (small ? "-s" : "") + ".png");
            try (FileOutputStream stream = new FileOutputStream(file)) {
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream);
            }
            return "file://" + file.getAbsolutePath();
        } catch (Exception failure) {
            return "";
        }
    }

    static StatusBarNotification findForView(String key) { return find(key); }

    static void openHistory(Context context) {
        context.startActivity(new Intent("android.settings.NOTIFICATION_HISTORY").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }

    /** App's notification settings, or the global notification settings for null. */
    static void openSettings(Context context, String packageName) {
        Intent intent = packageName == null || packageName.isEmpty()
                ? new Intent("android.settings.NOTIFICATION_SETTINGS")
                : new Intent(android.provider.Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                        .putExtra(android.provider.Settings.EXTRA_APP_PACKAGE, packageName);
        context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }

    static boolean dismiss(String key) {
        ShelfNotificationListener listener = active;
        if (listener == null) return false;
        listener.cancelNotification(key);
        return true;
    }

    static boolean dismissAll() {
        ShelfNotificationListener listener = active;
        if (listener == null) return false;
        listener.cancelAllNotifications();
        return true;
    }

    /** Opens the notification (its content intent) and cancels it when it is auto-cancel. */
    static boolean open(String key) {
        StatusBarNotification sbn = find(key);
        if (sbn == null || sbn.getNotification().contentIntent == null) return false;
        try {
            sbn.getNotification().contentIntent.send();
            if ((sbn.getNotification().flags & Notification.FLAG_AUTO_CANCEL) != 0) dismiss(key);
            return true;
        } catch (PendingIntent.CanceledException e) {
            return false;
        }
    }

    /** Fires action `index`; `reply` fills the action's RemoteInput(s) when given. */
    static boolean invokeAction(Context context, String key, int index, String reply) {
        StatusBarNotification sbn = find(key);
        if (sbn == null) return false;
        Notification.Action[] actions = sbn.getNotification().actions;
        if (actions == null || index < 0 || index >= actions.length) return false;
        Notification.Action action = actions[index];
        try {
            RemoteInput[] inputs = action.getRemoteInputs();
            if (reply != null && inputs != null && inputs.length > 0) {
                Intent fill = new Intent();
                Bundle results = new Bundle();
                for (RemoteInput input : inputs) results.putCharSequence(input.getResultKey(), reply);
                RemoteInput.addResultsToIntent(inputs, fill, results);
                action.actionIntent.send(context, 0, fill);
            } else {
                action.actionIntent.send();
            }
            return true;
        } catch (PendingIntent.CanceledException e) {
            return false;
        }
    }

    private static StatusBarNotification find(String key) {
        ShelfNotificationListener listener = active;
        if (listener == null) return null;
        try {
            StatusBarNotification[] found = listener.getActiveNotifications(new String[] {key});
            return found == null || found.length == 0 ? null : found[0];
        } catch (RuntimeException failure) {
            return null;
        }
    }
}
