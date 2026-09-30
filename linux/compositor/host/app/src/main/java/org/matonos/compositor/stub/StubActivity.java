package org.matonos.compositor.stub;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.widget.TextView;

/** Entry point class referenced by generated, code-free stub APK manifests. */
public final class StubActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String ref = null;
        int minimum = 1;
        try {
            ActivityInfo info = getPackageManager().getActivityInfo(getComponentName(),
                    android.content.pm.PackageManager.GET_META_DATA);
            if (info.metaData != null) {
                ref = info.metaData.getString(HostContract.META_FLATPAK_REF);
                minimum = info.metaData.getInt(HostContract.META_MIN_INTERFACE, 1);
            }
        } catch (Exception ignored) { }

        String message;
        if (ref == null || ref.trim().isEmpty()) {
            message = "This Flatpak stub has no application reference.";
        } else if (minimum > HostContract.getInterfaceVersion()) {
            message = "This app needs Linux host interface " + minimum
                    + "; installed host provides " + HostContract.getInterfaceVersion() + ".";
        } else {
            Intent host = new Intent().setClassName("org.matonos.compositor",
                    "org.matonos.compositor.MainActivity")
                    .putExtra(HostContract.EXTRA_FLATPAK_REF, ref);
            startActivity(host);
            finish();
            return;
        }
        TextView error = new TextView(this);
        error.setText(message);
        error.setPadding(32, 32, 32, 32);
        setContentView(error);
    }
}
