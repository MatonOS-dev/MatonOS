package org.matonos.settings.nativebridge;

import android.app.Service;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.ContentObserver;
import android.media.audiofx.DynamicsProcessing;
import android.net.Uri;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

/** Owns the output-mix dynamics effect while boost is above normal volume. */
public final class VolumeBoostService extends Service {
    private static final String TAG = "MatonVolumeBoost";
    private static final String SETTING = "matonos_volume_boost_percent";
    private static final int DEFAULT_PERCENT = 100;
    private static final int MAX_PERCENT = 200;
    private static final float MAX_BOOST_DB = 6.0f;

    private DynamicsProcessing effect;
    private ContentObserver settingObserver;

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
            return clamp(Integer.parseInt(stored));
        } catch (NumberFormatException error) {
            Log.e(TAG, "invalid stored boost percent=" + stored, error);
            return DEFAULT_PERCENT;
        }
    }

    public static int getPercentAndEnsureRunning(Context context) {
        int percent = getPercent(context);
        if (percent > DEFAULT_PERCENT) startAtBoot(context);
        return percent;
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
        if (!written) {
            throw new IllegalStateException("could not persist boost percent=" + percent);
        }
        Log.i(TAG, "saved boost percent=" + percent);
        try {
            context.startService(new Intent(context, VolumeBoostService.class));
        } catch (RuntimeException error) {
            Log.e(TAG, "could not start service for boost percent=" + percent, error);
            throw error;
        }
    }

    public static void startAtBoot(Context context) {
        try {
            context.startService(new Intent(context, VolumeBoostService.class));
        } catch (RuntimeException error) {
            Log.e(TAG, "could not start at boot", error);
        }
    }

    private static int clamp(int percent) {
        return Math.max(DEFAULT_PERCENT, Math.min(MAX_PERCENT, percent));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ContentResolver resolver = getContentResolver();
        settingObserver = new ContentObserver(new Handler(Looper.getMainLooper())) {
            @Override
            public void onChange(boolean selfChange, Uri uri) {
                applyStoredValue();
            }
        };
        try {
            resolver.registerContentObserver(
                    Settings.Secure.getUriFor(SETTING), false, settingObserver);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed observing secure setting " + SETTING, error);
            settingObserver = null;
        }
        Log.i(TAG, "service created; watching secure setting " + SETTING);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        applyStoredValue();
        return START_STICKY;
    }

    private void applyStoredValue() {
        int percent = getPercent(this);
        if (percent == DEFAULT_PERCENT) {
            releaseEffect();
            stopSelf();
            return;
        }
        float gainDb = (float) (Math.log(percent / 100.0) / Math.log(10.0) * 20.0);
        gainDb = Math.min(MAX_BOOST_DB, Math.max(0.0f, gainDb));
        try {
            if (effect == null) effect = createEffect();
            effect.setInputGainAllChannelsTo(gainDb);
            effect.setEnabled(true);
            Log.i(TAG, "applied output boost percent=" + percent + " gainDb=" + gainDb);
        } catch (RuntimeException error) {
            Log.e(TAG, "failed applying output boost percent=" + percent, error);
            releaseEffect();
            stopSelf();
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
        if (effect == null) return;
        DynamicsProcessing old = effect;
        effect = null;
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

    @Override
    public void onDestroy() {
        if (settingObserver != null) {
            try {
                getContentResolver().unregisterContentObserver(settingObserver);
            } catch (RuntimeException error) {
                Log.e(TAG, "failed unregistering secure setting observer", error);
            }
            settingObserver = null;
        }
        releaseEffect();
        Log.i(TAG, "service destroyed");
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
