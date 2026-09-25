package org.matonos.audiotest;

import android.app.Activity;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Bundle;
import android.widget.TextView;

public final class ToneActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView status = new TextView(this);
        status.setText("Playing 440 Hz test tone through AudioTrack…");
        status.setTextSize(20);
        status.setPadding(32, 32, 32, 32);
        setContentView(status);
        new Thread(() -> playTone(status), "maton-audio-tone").start();
    }

    private void playTone(TextView status) {
        final int sampleRate = 48000;
        final int channels = 2;
        final int frames = sampleRate * 3;
        final int bufferFrames = 1024;
        short[] samples = new short[bufferFrames * channels];
        AudioFormat format = new AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .build();
        AudioTrack track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setAudioFormat(format)
                .setBufferSizeInBytes(bufferFrames * channels * Short.BYTES)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        try {
            track.play();
            for (int offset = 0; offset < frames; offset += bufferFrames) {
                int count = Math.min(bufferFrames, frames - offset);
                for (int frame = 0; frame < count; frame++) {
                    short value = (short) (Math.sin(2.0 * Math.PI * 440.0
                            * (offset + frame) / sampleRate) * 6000);
                    samples[frame * channels] = value;
                    samples[frame * channels + 1] = value;
                }
                int written = track.write(samples, 0, count * channels,
                        AudioTrack.WRITE_BLOCKING);
                if (written <= 0) break;
            }
        } catch (IllegalStateException ignored) {
            // Show the failed stream state on the test screen.
        } finally {
            track.release();
            runOnUiThread(() -> status.setText("AudioTrack test finished; inspect QEMU WAV."));
        }
    }
}
