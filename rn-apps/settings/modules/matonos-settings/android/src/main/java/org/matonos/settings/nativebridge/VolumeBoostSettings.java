/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.settings.nativebridge;

import android.content.Context;
import android.provider.Settings;
import android.util.Log;

/** Read and write the boost preference; persistent SystemUI owns the audio effect. */
public final class VolumeBoostSettings {
    private static final String TAG = "MatonVolumeBoost";
    private static final String SETTING = "matonos_volume_boost_percent";
    private static final int DEFAULT_PERCENT = 100;
    private static final int MAX_PERCENT = 200;

    private VolumeBoostSettings() {}

    public static int getPercent(Context context) {
        final String stored;
        try {
            stored = Settings.Secure.getString(context.getContentResolver(), SETTING);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed reading secure setting " + SETTING, error);
            throw error;
        }
        if (stored == null) return DEFAULT_PERCENT;
        try {
            return Math.max(DEFAULT_PERCENT, Math.min(MAX_PERCENT, Integer.parseInt(stored)));
        } catch (NumberFormatException error) {
            Log.e(TAG, "invalid stored boost percent=" + stored, error);
            return DEFAULT_PERCENT;
        }
    }

    public static void setPercent(Context context, int percent) {
        if (percent < DEFAULT_PERCENT || percent > MAX_PERCENT) {
            throw new IllegalArgumentException("boost percent out of range: " + percent);
        }
        final boolean written;
        try {
            written = Settings.Secure.putInt(context.getContentResolver(), SETTING, percent);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed writing boost percent=" + percent, error);
            throw error;
        }
        if (!written) throw new IllegalStateException("could not persist boost percent=" + percent);
    }
}
