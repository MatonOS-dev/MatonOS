package org.matonos.settings.gapps;

import android.app.Activity;
import android.os.Bundle;

import expo.modules.kotlin.Promise;

public final class GappsPickerActivity extends Activity {
    static volatile Promise promise;
    static volatile GappsInstaller.PendingInstall pendingInstall;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        GappsInstaller.launchPicker(this);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 7412) GappsInstaller.handlePickerResult(this, resultCode, data);
    }
}
