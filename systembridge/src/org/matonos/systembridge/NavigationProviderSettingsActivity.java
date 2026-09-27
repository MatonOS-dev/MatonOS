package org.matonos.systembridge;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;

/** Explicit user consent screen for selecting the system-hosted navigation provider. */
public final class NavigationProviderSettingsActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String provider = getIntent().getStringExtra("providerPackage");
        if (provider == null || provider.isEmpty()) provider = NavigationBarWindow.DEFAULT_PROVIDER;
        final String selected = provider;
        String certificate = SystemBridgeService.certificateFor(this, selected);
        if (NavigationBarWindow.DEFAULT_PROVIDER.equals(selected)) {
            new AlertDialog.Builder(this)
                    .setTitle("Built-in navigation bar")
                    .setMessage("MatonOS Shelf is built in and its signing certificate is pinned by the image. "
                            + "It is enabled by default. You can restore it or revoke its built-in navigation capability.\n\n"
                            + "Certificate: " + certificate)
                    .setPositiveButton("Use Shelf", (dialog, which) -> {
                        try { SystemBridgeService.selectNavigationProvider(this, selected, true); }
                        catch (RuntimeException failure) { showError(failure); }
                        finish();
                    })
                    .setNegativeButton("Close", (dialog, which) -> finish())
                    .setNeutralButton("Revoke", (dialog, which) -> {
                        SystemBridgeService.selectNavigationProvider(this, "", false);
                        finish();
                    })
                    .show();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Navigation bar provider")
                .setMessage("Allow " + selected + " to provide the system navigation bar?\n\n"
                        + "The selected app can request Back, Home and Recents actions. Its signing certificate "
                        + "will be pinned until you revoke this choice.\n\nCertificate: " + certificate)
                .setPositiveButton("Allow", (dialog, which) -> {
                    try {
                        SystemBridgeService.selectNavigationProvider(this, selected, true);
                        finish();
                    } catch (RuntimeException failure) {
                        showError(failure);
                    }
                })
                .setNegativeButton("Not now", (dialog, which) -> finish())
                .setNeutralButton("Revoke", (dialog, which) -> {
                    SystemBridgeService.selectNavigationProvider(this, "", false);
                    finish();
                })
                .setOnCancelListener(dialog -> finish())
                .show();
    }

    private void showError(RuntimeException failure) {
        new AlertDialog.Builder(this).setTitle("Provider unavailable")
                .setMessage(failure.getMessage()).setPositiveButton("OK", (d, w) -> finish()).show();
    }
}
