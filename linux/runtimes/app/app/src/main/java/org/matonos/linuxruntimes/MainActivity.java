package org.matonos.linuxruntimes;

import android.os.Bundle;

/**
 * MatonOS Linux Runtimes: a code-free preinstalled app that owns the UID
 * for Flatpak SYSTEM installations (runtimes). No UI, no permissions, no
 * privileged access. Its only purpose is to be an installed package with
 * a stable UID that linuxd uses for FLATPAK_SYSTEM_DIR ownership.
 */
public class MainActivity extends android.app.Activity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // This app has no UI components. It exists solely to own a UID for
        // Flatpak runtime storage on /data/matonos/linux/apps/<runtimes uid>/
        finish();
    }
}
