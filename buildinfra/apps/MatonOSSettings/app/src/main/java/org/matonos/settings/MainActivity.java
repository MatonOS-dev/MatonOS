package org.matonos.settings;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.content.Intent;
import org.matonos.client.MatonOS;
import org.matonos.client.MatonosClient;
import org.matonos.client.SleepClient;

public final class MainActivity extends Activity {
    private TextView status;
    private MatonosClient client;
    private SleepClient sleep;
    private final SleepClient.StateListener stateListener = state -> runOnUiThread(() ->
            status.setText("Sleep daemon connected\n" + state));

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        client = MatonOS.requireBridge(this, new String[]{"sleep"}, new String[]{"sleep"}, ready -> {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(32, 32, 32, 32);
        status = new TextView(this);
        status.setText("Connecting to MatonOS system bridge…");
        Button back = new Button(this);
        back.setText("Send BACK through bridge");
        back.setOnClickListener(v -> {
            if (client == null || !client.isAvailable()) return;
            new Thread(() -> {
                MatonosClient.Result<Boolean> result = client.injectBackKey();
                runOnUiThread(() -> status.setText(result.available && result.value ? "BACK injected" : "BACK unavailable: " + result.reason));
            }).start();
        });
        Button trust = new Button(this); trust.setText("Trusted developer apps");
        trust.setOnClickListener(v -> startActivity(new Intent(MatonOS.TRUST_SETTINGS)
                .setComponent(new android.content.ComponentName("org.matonos.systembridge",
                        "org.matonos.systembridge.SystemBridgeService$TrustedAppsActivity"))));
        page.addView(status);
        page.addView(back);
        page.addView(trust);
        setContentView(page);
        sleep = new SleepClient(client);
        MatonosClient.Result<SleepClient.State> current = sleep.getState();
        status.setText(current.available ? "Sleep daemon connected\n" + current.value : "Sleep daemon unavailable: " + current.reason);
        sleep.subscribe(stateListener);
        });
    }

    @Override protected void onDestroy() {
        if (sleep != null) try { sleep.unsubscribe(stateListener); } catch (Exception ignored) { }
        if (client != null) client.close();
        super.onDestroy();
    }
}
