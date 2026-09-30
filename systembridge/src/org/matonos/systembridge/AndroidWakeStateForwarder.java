package org.matonos.systembridge;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioManager;
import android.media.AudioPlaybackConfiguration;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.ServiceManager;
import android.provider.Settings;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.util.List;

import vendor.matonos.channel.IChannel;

/** Polls privileged platform power state and forwards its suspend blockers to sleepd. */
final class AndroidWakeStateForwarder {
    private static final String TAG = "MatonSystemBridge";
    private static final long POLL_MS = 2000;
    private static final int MAX_DUMP_BYTES = 256 * 1024;

    private final Context context;
    private HandlerThread thread;
    private Handler handler;
    private boolean lastKnownBlocked;
    private int lastWakeLockCount = -1;

    AndroidWakeStateForwarder(Context context) { this.context = context; }

    void start() {
        if (thread != null) return;
        thread = new HandlerThread("MatonSleepWakeForwarder");
        thread.start();
        handler = new Handler(thread.getLooper());
        // Fail safe during startup: sleepd must not suspend before the first
        // complete read of PowerManager's live wake-lock table. Keep Binder
        // work off the bridge's main thread so boot cannot wait on the daemon.
        handler.post(() -> {
            forward(true, 0, false, false);
            poll.run();
        });
    }

    void stop() {
        if (handler != null) handler.removeCallbacksAndMessages(null);
        if (thread != null) thread.quitSafely();
        handler = null;
        thread = null;
    }

    private final Runnable poll = new Runnable() {
        @Override public void run() {
            try {
                WakeState state = readWakeState();
                forward(state.blocked, state.wakeLocks, state.audioActive, state.stayAwake);
            } catch (Exception e) {
                Log.w(TAG, "Cannot inspect Android wake state; sleep remains blocked", e);
                forward(true, Math.max(0, lastWakeLockCount), false, false);
            }
            if (handler != null) handler.postDelayed(this, POLL_MS);
        }
    };

    private WakeState readWakeState() throws Exception {
        IBinder power = ServiceManager.checkService("power");
        if (power == null) throw new IllegalStateException("PowerManager service unavailable");
        String dump = dumpPowerService(power);
        int count = countActiveWakeLocks(dump);

        AudioManager audio = context.getSystemService(AudioManager.class);
        List<AudioPlaybackConfiguration> players = audio == null
                ? null : audio.getActivePlaybackConfigurations();
        boolean audioActive = false;
        if (players != null) {
            for (AudioPlaybackConfiguration player : players) {
                if (player.getPlayerState() == AudioPlaybackConfiguration.PLAYER_STATE_STARTED) {
                    audioActive = true;
                    break;
                }
            }
        }

        Intent battery = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int plugged = battery == null ? 0 : battery.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0);
        int stayOnSetting = Settings.Global.getInt(context.getContentResolver(),
                Settings.Global.STAY_ON_WHILE_PLUGGED_IN, 0);
        boolean stayAwake = (stayOnSetting & plugged) != 0;
        return new WakeState(count, audioActive, stayAwake,
                count > 0 || audioActive || stayAwake);
    }

    private static String dumpPowerService(IBinder power) throws Exception {
        ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
        Thread writer = new Thread(() -> {
            try (ParcelFileDescriptor output = pipe[1]) {
                power.dump(output.getFileDescriptor(), new String[0]);
            } catch (Exception e) {
                Log.w(TAG, "PowerManager dump failed", e);
            }
        }, "MatonPowerDump");
        writer.setDaemon(true);
        writer.start();

        try (ParcelFileDescriptor input = pipe[0];
             FileInputStream stream = new FileInputStream(input.getFileDescriptor());
             ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int read;
            while ((read = stream.read(buffer)) != -1) {
                if (bytes.size() + read > MAX_DUMP_BYTES)
                    throw new IllegalStateException("PowerManager dump exceeds size limit");
                bytes.write(buffer, 0, read);
            }
            writer.join(1000);
            String result = bytes.toString("UTF-8");
            if (result.isEmpty()) throw new IllegalStateException("Empty PowerManager dump");
            return result;
        }
    }

    private static int countActiveWakeLocks(String dump) {
        int heading = dump.indexOf("Wake Locks: size=");
        if (heading < 0) throw new IllegalStateException("Wake-lock section missing from power dump");
        int end = dump.indexOf("Suspend Blockers:", heading);
        if (end < 0) throw new IllegalStateException("Incomplete wake-lock section in power dump");
        int count = 0;
        int cursor = heading;
        while ((cursor = dump.indexOf('\n', cursor)) >= 0 && cursor < end) {
            int lineStart = cursor + 1;
            int lineEnd = dump.indexOf('\n', lineStart);
            if (lineEnd < 0 || lineEnd > end) lineEnd = end;
            String line = dump.substring(lineStart, lineEnd).trim();
            if (!line.isEmpty() && !line.startsWith("Suspend Blockers:")
                    && !line.contains("DISABLED") && !line.contains("isFrozen=true")) {
                count++;
            }
            cursor = lineStart;
        }
        return count;
    }

    private void forward(boolean blocked, int wakeLocks, boolean audioActive, boolean stayAwake) {
        try {
            IBinder service = ServiceManager.checkService("vendor.matonos.channel.IChannel/sleep");
            IChannel channel = IChannel.Stub.asInterface(service);
            if (channel == null) return;
            JSONObject args = new JSONObject();
            args.put("blocked", blocked);
            args.put("wakeLockCount", wakeLocks);
            args.put("audioActive", audioActive);
            args.put("stayAwake", stayAwake);
            String result = channel.call("set_wake_state", args.toString());
            if (result == null || !new JSONObject(result).optBoolean("ok", false))
                throw new IllegalStateException("sleepd rejected wake state");
            if (lastKnownBlocked != blocked || lastWakeLockCount != wakeLocks) {
                Log.i(TAG, "Forwarded Android wake state: blocked=" + blocked
                        + " wakeLocks=" + wakeLocks + " audio=" + audioActive
                        + " stayAwake=" + stayAwake);
            }
            lastKnownBlocked = blocked;
            lastWakeLockCount = wakeLocks;
        } catch (Exception e) {
            Log.w(TAG, "Cannot forward Android wake state to sleepd", e);
        }
    }

    private static final class WakeState {
        final int wakeLocks;
        final boolean audioActive;
        final boolean stayAwake;
        final boolean blocked;
        WakeState(int wakeLocks, boolean audioActive, boolean stayAwake, boolean blocked) {
            this.wakeLocks = wakeLocks;
            this.audioActive = audioActive;
            this.stayAwake = stayAwake;
            this.blocked = blocked;
        }
    }
}
