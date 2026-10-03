// SPDX-License-Identifier: Apache-2.0
package org.matonos.settings.nativebridge;

import android.app.PendingIntent;
import android.content.Intent;
import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Opens Android's volume sliders without changing the current media volume. */
public final class MediaVolumeTileService extends TileService {
    @Override
    public void onStartListening() {
        Tile tile = getQsTile();
        if (tile != null) {
            // This is a panel shortcut, not a mute toggle.
            tile.setState(Tile.STATE_INACTIVE);
            tile.setLabel(getString(R.string.media_volume_tile_label));
            tile.updateTile();
        }
    }

    @Override
    public void onClick() {
        if (isLocked()) {
            unlockAndRun(this::openVolumePanel);
        } else {
            openVolumePanel();
        }
    }

    @SuppressWarnings("deprecation")
    private void openVolumePanel() {
        // Collapse QS before showing the native volume UI, so its slider is
        // visible and usable with a mouse even when no media session is active.
        Intent intent = new Intent(this, MediaVolumeActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        } else {
            startActivityAndCollapse(intent);
        }
    }
}
