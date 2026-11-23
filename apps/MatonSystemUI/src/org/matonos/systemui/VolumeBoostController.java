/* SPDX-License-Identifier: Apache-2.0 */
package org.matonos.systemui;

import android.content.ContentResolver;
import android.content.Context;
import android.database.ContentObserver;
import android.media.audiofx.DynamicsProcessing;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

/** Keeps the output-mix boost alive for the lifetime of persistent SystemUI. */
final class VolumeBoostController {
    private static final String TAG = "MatonVolumeBoost";
    private static final String SETTING = "matonos_volume_boost_percent";
    private static final int DEFAULT_PERCENT = 100;
    private static final int MAX_PERCENT = 200;
    private static final float MAX_BOOST_DB = 6.0f;

    private final ContentResolver mResolver;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final ContentObserver mObserver;
    private DynamicsProcessing mEffect;

    VolumeBoostController(Context context) {
        mResolver = context.getContentResolver();
        mObserver = new ContentObserver(mHandler) {
            @Override
            public void onChange(boolean selfChange, Uri uri) {
                applyStoredValue();
            }
        };
        try {
            mResolver.registerContentObserver(
                    Settings.Secure.getUriFor(SETTING), false, mObserver);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed observing secure setting " + SETTING, error);
        }
        // Don't do audio-service work on the SystemUI startup path.
        if (!mHandler.post(this::applyStoredValue)) {
            Log.e(TAG, "main handler rejected initial boost apply");
        }
    }

    private void applyStoredValue() {
        int percent = readPercent();
        if (percent == DEFAULT_PERCENT) {
            releaseEffect();
            return;
        }
        float gainDb = (float) (Math.log(percent / 100.0) / Math.log(10.0) * 20.0);
        gainDb = Math.min(MAX_BOOST_DB, Math.max(0.0f, gainDb));
        try {
            if (mEffect == null) mEffect = createEffect();
            mEffect.setInputGainAllChannelsTo(gainDb);
            mEffect.setEnabled(true);
            Log.i(TAG, "applied output boost percent=" + percent + " gainDb=" + gainDb);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed applying output boost percent=" + percent, error);
            releaseEffect();
        }
    }

    private int readPercent() {
        final String stored;
        try {
            stored = Settings.Secure.getString(mResolver, SETTING);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed reading secure setting " + SETTING, error);
            return DEFAULT_PERCENT;
        }
        if (stored == null) return DEFAULT_PERCENT;
        try {
            return Math.max(DEFAULT_PERCENT, Math.min(MAX_PERCENT, Integer.parseInt(stored)));
        } catch (NumberFormatException error) {
            Log.e(TAG, "invalid stored boost percent=" + stored, error);
            return DEFAULT_PERCENT;
        }
    }

    private static DynamicsProcessing createEffect() {
        DynamicsProcessing.Limiter limiter = new DynamicsProcessing.Limiter(
                true, true, 0, 1.0f, 80.0f, 100.0f, -1.0f, 0.0f);
        DynamicsProcessing.Config config = new DynamicsProcessing.Config.Builder(
                DynamicsProcessing.VARIANT_FAVOR_TIME_RESOLUTION,
                2,
                false, 0,
                false, 0,
                false, 0,
                true)
                .setLimiterAllChannelsTo(limiter)
                .build();
        DynamicsProcessing created = new DynamicsProcessing(0, 0, config);
        try {
            created.setEnabled(true);
            return created;
        } catch (RuntimeException error) {
            try {
                created.release();
            } catch (RuntimeException releaseError) {
                Log.e(TAG, "failed releasing partially-created effect", releaseError);
            }
            throw error;
        }
    }

    private void releaseEffect() {
        if (mEffect == null) return;
        DynamicsProcessing old = mEffect;
        mEffect = null;
        try {
            old.setEnabled(false);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed disabling output mix effect", error);
        }
        try {
            old.release();
            Log.i(TAG, "released output mix effect");
        } catch (RuntimeException error) {
            Log.e(TAG, "failed releasing output mix effect", error);
        }
    }
}
