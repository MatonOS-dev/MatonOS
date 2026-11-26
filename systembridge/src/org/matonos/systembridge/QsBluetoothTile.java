package org.matonos.systembridge;

import android.app.ActivityManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Resources;
import android.provider.Settings;
import android.util.Log;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Keeps the quick-settings Bluetooth tile out of the tile list while no controller exists.
 * The stock stack aborts when enabled without a controller, so the toggle must not be offered.
 * Only a tile this class removed is put back, at its old position.
 */
final class QsBluetoothTile {
    private static final String TAG = "MatonSystemBridge";
    private static final String TILES = "sysui_qs_tiles";
    private static final String TILE = "bt";
    private static final String PREFS = "qs-bluetooth-tile";
    private static final String HIDDEN_INDEX = "hidden_index_user_";

    private QsBluetoothTile() {}

    static void sync(Context context, boolean controllerPresent) {
        int user = ActivityManager.getCurrentUser();
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String key = HIDDEN_INDEX + user;
        List<String> tiles = currentTiles(context, user);
        int index = tiles.indexOf(TILE);
        if (!controllerPresent && index >= 0) {
            tiles.remove(index);
            write(context, user, tiles);
            prefs.edit().putInt(key, index).apply();
            Log.i(TAG, "No Bluetooth controller: removed QS tile at " + index + " for user " + user);
        } else if (controllerPresent && index < 0 && prefs.contains(key)) {
            tiles.add(Math.min(prefs.getInt(key, 0), tiles.size()), TILE);
            write(context, user, tiles);
            prefs.edit().remove(key).apply();
            Log.i(TAG, "Bluetooth controller present: restored QS tile for user " + user);
        }
    }

    private static List<String> currentTiles(Context context, int user) {
        String saved = Settings.Secure.getStringForUser(context.getContentResolver(), TILES, user);
        if (saved == null || saved.isEmpty()) saved = systemUiDefault(context);
        return new ArrayList<>(Arrays.asList(saved.split(",")));
    }

    // SystemUI falls back to its default list while the setting is unset (fresh user data).
    private static String systemUiDefault(Context context) {
        try {
            Resources res = context.getPackageManager().getResourcesForApplication("com.android.systemui");
            int id = res.getIdentifier("quick_settings_tiles_default", "string", "com.android.systemui");
            if (id == 0) throw new IllegalStateException("quick_settings_tiles_default not found");
            return res.getString(id);
        } catch (Exception e) {
            throw new IllegalStateException("Cannot read SystemUI default QS tiles", e);
        }
    }

    private static void write(Context context, int user, List<String> tiles) {
        if (!Settings.Secure.putStringForUser(context.getContentResolver(), TILES, String.join(",", tiles), user)) {
            throw new IllegalStateException("Writing " + TILES + " failed for user " + user);
        }
    }
}
