// SPDX-License-Identifier: Apache-2.0
package org.matonos.settings.nativebridge;

import android.app.Activity;
import android.media.AudioManager;

/** Transparent QS trampoline: let SystemUI own the actual media volume slider. */
public final class MediaVolumeActivity extends Activity {
    @Override
    protected void onResume() {
        super.onResume();
        AudioManager audio = getSystemService(AudioManager.class);
        if (audio != null) {
            // ADJUST_SAME requests the UI without raising, lowering or toggling mute.
            // An explicit stream ensures idle PCs get the media slider too.
            audio.adjustStreamVolume(AudioManager.STREAM_MUSIC,
                    AudioManager.ADJUST_SAME, AudioManager.FLAG_SHOW_UI);
        }
        finish();
    }
}
