package org.matonos.compositor;

import android.app.Activity;
import android.content.ComponentName;
import android.content.BroadcastReceiver;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;

public final class WindowActivity extends Activity implements SurfaceHolder.Callback {
    static final String EXTRA_WINDOW_ID = "window_id";
    static final String EXTRA_WIDTH = "window_width";
    static final String EXTRA_HEIGHT = "window_height";
    static final String EXTRA_START_DEMO = "start_demo";
    static final String ACTION_CLOSE_WINDOW = "org.matonos.compositor.CLOSE_WINDOW";
    private int id; private Surface surface; private ICompositor compositor; private SurfaceView view; private boolean attached, demoLaunched;
    private final ServiceConnection connection = new ServiceConnection() {
        public void onServiceConnected(ComponentName n, IBinder b) { compositor = ICompositor.Stub.asInterface(b); attachIfReady(); }
        public void onServiceDisconnected(ComponentName n) { compositor = null; }
    };
    private final BroadcastReceiver closeReceiver = new BroadcastReceiver() {
        @Override public void onReceive(android.content.Context context, android.content.Intent intent) {
            if (intent.getIntExtra(EXTRA_WINDOW_ID, -1) == id) finish();
        }
    };
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state); id = getIntent().getIntExtra(EXTRA_WINDOW_ID, 1); view = new SurfaceView(this); view.getHolder().addCallback(this);
        view.setFocusableInTouchMode(true); setContentView(view, new ViewGroup.LayoutParams(-1, -1));
        bindService(new android.content.Intent(this, CompositorService.class), connection, BIND_AUTO_CREATE);
        if (android.os.Build.VERSION.SDK_INT >= 33) registerReceiver(closeReceiver, new IntentFilter(ACTION_CLOSE_WINDOW), RECEIVER_NOT_EXPORTED);
        else registerReceiver(closeReceiver, new IntentFilter(ACTION_CLOSE_WINDOW));
    }
    private void attachIfReady() {
        if (compositor == null || surface == null || !surface.isValid()) return;
        if (attached) return;
        try {
            compositor.attachWindow(id, surface, view.getWidth(), view.getHeight()); attached = true; view.requestFocus();
            if (!demoLaunched && getIntent().getBooleanExtra(EXTRA_START_DEMO, false)) { compositor.launchDemo(); demoLaunched = true; }
        } catch (Exception ignored) { }
    }
    @Override public void surfaceCreated(SurfaceHolder h) { surface = h.getSurface(); attachIfReady(); }
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) { surface = h.getSurface(); attachIfReady(); try { if(compositor!=null && attached) compositor.resizeWindow(id,w,ht); } catch(Exception ignored){} }
    @Override public void surfaceDestroyed(SurfaceHolder h) { try { if(compositor!=null && attached) compositor.detachWindow(id); } catch(Exception ignored){} attached = false; surface = null; }
    @Override public boolean dispatchKeyEvent(KeyEvent e) { try { if(compositor!=null) compositor.keyEvent(id,e.getKeyCode(),e.getScanCode(),e.getAction(),e.getMetaState(),e.getEventTime()*1000000L); } catch(Exception ignored){} return true; }
    @Override public boolean onTouchEvent(MotionEvent e) { try { if(compositor!=null) compositor.motionEvent(id,e.getX(),e.getY(),e.getAxisValue(MotionEvent.AXIS_VSCROLL),e.getAxisValue(MotionEvent.AXIS_HSCROLL),e.getActionMasked(),e.getButtonState(),e.getEventTime()*1000000L); } catch(Exception ignored){} return true; }
    @Override public boolean onGenericMotionEvent(MotionEvent e) { return onTouchEvent(e); }
    @Override protected void onDestroy() { unregisterReceiver(closeReceiver); unbindService(connection); super.onDestroy(); }
}
