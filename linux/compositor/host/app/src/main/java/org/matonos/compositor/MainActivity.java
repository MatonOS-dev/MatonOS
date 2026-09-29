package org.matonos.compositor;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        startForegroundService(new Intent(this, CompositorService.class));
        LinearLayout layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL); layout.setPadding(24, 24, 24, 24);
        TextView label = new TextView(this);
        String ref = getIntent().getStringExtra("org.matonos.linuxhost.FLATPAK_REF");
        label.setText(ref == null
                ? "Wayland compositor is starting. Launch the bundled test client to open a window."
                : "Flatpak app " + ref + " cannot start yet: the Flatpak runtime launcher is not wired to the compositor.");
        layout.addView(label);
        Button launch = new Button(this); launch.setText("Open Wayland test window"); launch.setOnClickListener(v -> {
            startActivity(new Intent(this, WindowActivity.class).putExtra(WindowActivity.EXTRA_WINDOW_ID, 1)
                    .putExtra(WindowActivity.EXTRA_START_DEMO, true));
        }); layout.addView(launch); setContentView(layout);
    }
}
