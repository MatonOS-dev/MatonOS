package org.matonos.systembridge;

import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.provider.MediaStore;
import android.util.Log;

import java.io.FileInputStream;
import java.io.OutputStream;

/** Publish the bundled song through MediaStore once per Android user. */
final class PreinstalledMusic {
    private PreinstalledMusic() { }

    static synchronized void install(Context context) {
        var preferences = context.getSharedPreferences("preinstalled-music", Context.MODE_PRIVATE);
        if (preferences.getBoolean("ovmuz-v1", false)) return;
        Uri collection = MediaStore.Audio.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
        Uri item = null;
        try {
            // Recover a published item if the process died before saving the marker.
            try (var cursor = context.getContentResolver().query(collection,
                    new String[]{MediaStore.Audio.Media._ID},
                    MediaStore.Audio.Media.DISPLAY_NAME + "=? AND "
                            + MediaStore.Audio.Media.RELATIVE_PATH + "=? AND "
                            + MediaStore.Audio.Media.OWNER_PACKAGE_NAME + "=? AND "
                            + MediaStore.Audio.Media.IS_PENDING + "=0",
                    new String[]{"ovmuz.mp3", "Music/MatonOS/", context.getPackageName()}, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    preferences.edit().putBoolean("ovmuz-v1", true).commit();
                    return;
                }
            }
            ContentValues values = new ContentValues();
            values.put(MediaStore.Audio.Media.DISPLAY_NAME, "ovmuz.mp3");
            values.put(MediaStore.Audio.Media.MIME_TYPE, "audio/mpeg");
            values.put(MediaStore.Audio.Media.RELATIVE_PATH, "Music/MatonOS/");
            values.put(MediaStore.Audio.Media.IS_MUSIC, 1);
            values.put(MediaStore.Audio.Media.IS_PENDING, 1);
            item = context.getContentResolver().insert(collection, values);
            if (item == null) throw new java.io.IOException("MediaStore insertion failed");
            try (var input = new FileInputStream("/product/media/matonos/music/ovmuz.mp3");
                    OutputStream output = context.getContentResolver().openOutputStream(item, "w")) {
                if (output == null) throw new java.io.IOException("MediaStore output unavailable");
                input.transferTo(output);
            }
            values.clear();
            values.put(MediaStore.Audio.Media.IS_PENDING, 0);
            if (context.getContentResolver().update(item, values, null, null) != 1)
                throw new java.io.IOException("MediaStore publication failed");
            preferences.edit().putBoolean("ovmuz-v1", true).commit();
        } catch (Exception error) {
            if (item != null) {
                try { context.getContentResolver().delete(item, null, null); }
                catch (Exception ignored) { }
            }
            Log.w("MatonMusic", "Bundled music will be retried at next boot", error);
        }
    }
}
