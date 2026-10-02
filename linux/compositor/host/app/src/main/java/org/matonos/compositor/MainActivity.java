package org.matonos.compositor;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    private TextView label;
    private boolean bound, listening;
    private volatile boolean destroyed;
    private final android.content.BroadcastReceiver opened = new android.content.BroadcastReceiver() {
        @Override public void onReceive(android.content.Context context, Intent intent) {
            if (getIntent().getStringExtra("org.matonos.linuxhost.FLATPAK_REF") != null) finish();
        }
    };
    private final ServiceConnection connection = new ServiceConnection() {
        public void onServiceConnected(ComponentName name, IBinder binder) {
            ICompositor engine = ICompositor.Stub.asInterface(binder);
            String ref = getIntent().getStringExtra("org.matonos.linuxhost.FLATPAK_REF");
            if (ref == null) return;
            label.setText("Starting application…");
            new Thread(() -> {
                try {
                    String reply = engine.launchFlatpak(ref);
                    org.json.JSONObject result = new org.json.JSONObject(reply);
                    runOnUiThread(() -> {
                        if (result.optBoolean("ok")) label.setText("Waiting for the application window…");
                        else label.setText(result.optString("error", "Application could not start"));
                    });
                    if(result.optBoolean("ok")) {
                        for(int i=0;i<30&&!destroyed;i++) {
                            Thread.sleep(1000);
                            org.json.JSONObject status=new org.json.JSONObject(engine.getLaunchStatus(ref));
                            if(!status.optBoolean("ok")) {
                                runOnUiThread(()->label.setText(status.optString("error","Application exited")));
                                return;
                            }
                        }
                        if(!destroyed)runOnUiThread(()->label.setText("Application is running, but no window has appeared."));
                    }
                } catch (Exception e) {
                    android.util.Log.e("MatonCompositor", "Launch failed for " + ref, e);
                    String detail = e instanceof android.os.DeadObjectException
                        ? "The compositor stopped unexpectedly."
                        : e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
                    runOnUiThread(() -> label.setText("Cannot start application: " + detail));
                }
            }, "flatpak-launch").start();
        }
        public void onServiceDisconnected(ComponentName name) { label.setText("Compositor disconnected"); }
    };
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String stub=getIntent().getStringExtra("org.matonos.linuxhost.STUB_PACKAGE");
        if(stub!=null && stub.startsWith("org.matonos.flatpak.stub.")) {
            try {
                android.content.pm.ApplicationInfo app=getPackageManager().getApplicationInfo(stub,0);
                android.graphics.drawable.Drawable icon=app.loadIcon(getPackageManager());
                android.graphics.Bitmap bitmap=android.graphics.Bitmap.createBitmap(192,192,android.graphics.Bitmap.Config.ARGB_8888);
                icon.setBounds(0,0,192,192);icon.draw(new android.graphics.Canvas(bitmap));
                setTaskDescription(new android.app.ActivityManager.TaskDescription(app.loadLabel(getPackageManager()).toString(),bitmap));
            } catch(Exception ignored) { }
        }
        LinearLayout layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL); layout.setPadding(24,24,24,24);
        label = new TextView(this); label.setText("Wayland compositor is starting…"); layout.addView(label);
        Button demo = new Button(this); demo.setText("Open Wayland test window");
        demo.setOnClickListener(v -> startActivity(new Intent(this, WindowActivity.class)
                .putExtra(WindowActivity.EXTRA_WINDOW_ID, 1).putExtra(WindowActivity.EXTRA_START_DEMO,true)));
        if (getIntent().getStringExtra("org.matonos.linuxhost.FLATPAK_REF") == null) layout.addView(demo);
        setContentView(layout);
        if (android.os.Build.VERSION.SDK_INT >= 33)
            registerReceiver(opened,new android.content.IntentFilter(CompositorService.ACTION_WINDOW_OPENED),RECEIVER_NOT_EXPORTED);
        else registerReceiver(opened,new android.content.IntentFilter(CompositorService.ACTION_WINDOW_OPENED));
        listening=true;
        startForegroundService(new Intent(this, CompositorService.class));
        bound = bindService(new Intent(this, CompositorService.class), connection, BIND_AUTO_CREATE);
        if (!bound) label.setText("Cannot connect to the compositor");
    }
    @Override protected void onDestroy() { destroyed=true; if (bound) unbindService(connection); if(listening) unregisterReceiver(opened); super.onDestroy(); }
}
