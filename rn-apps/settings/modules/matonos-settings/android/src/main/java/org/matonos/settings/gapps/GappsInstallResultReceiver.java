package org.matonos.settings.gapps;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

import java.util.List;

import expo.modules.kotlin.Promise;

public final class GappsInstallResultReceiver extends BroadcastReceiver {
    static volatile Promise uninstallPromise;
    static volatile List<String> uninstallRemaining;

    @Override public void onReceive(Context context, Intent intent) {
        if ("org.matonos.settings.GAPPS_UNINSTALL_RESULT".equals(intent.getAction())) {
            GappsInstaller.onUninstallResult(context, intent);
        } else {
            GappsInstaller.onInstallResult(context, intent);
        }
    }
}
