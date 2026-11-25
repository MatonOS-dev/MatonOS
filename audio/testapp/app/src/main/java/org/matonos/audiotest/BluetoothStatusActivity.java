// Copyright 2026 MatonOS
// SPDX-License-Identifier: Apache-2.0
package org.matonos.audiotest;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothStatusCodes;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/** Public-API probe; also works on a VM with no Bluetooth controller. */
public final class BluetoothStatusActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        BluetoothManager manager = getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        String result;
        if (adapter == null) {
            result = "Bluetooth adapter unavailable; LE Audio support cannot be tested";
        } else {
            int unicast = adapter.isLeAudioSupported();
            int source = adapter.isLeAudioBroadcastSourceSupported();
            int assistant = adapter.isLeAudioBroadcastAssistantSupported();
            result = "LE_AUDIO=" + unicast + " BROADCAST_SOURCE=" + source
                    + " BROADCAST_ASSISTANT=" + assistant
                    + " FEATURE_SUPPORTED=" + BluetoothStatusCodes.FEATURE_SUPPORTED;
        }
        Log.i("MatonBtAudioProbe", result);
        TextView view = new TextView(this);
        view.setText(result);
        setContentView(view);
    }
}
