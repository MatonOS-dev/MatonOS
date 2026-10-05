package org.matonos.compositor.stub;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.IBinder;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.TextView;
import org.json.JSONObject;
import org.matonos.compositor.IEmbeddedHost;
import org.matonos.compositor.IEmbeddedSession;
import org.matonos.compositor.IEmbeddedWindowListener;

/** Shared activity code runs in the generated app's own package and task. */
public final class StubActivity extends Activity implements SurfaceHolder.Callback {
    private static final String WINDOW = "org.matonos.linuxhost.WINDOW_ID";
    // Kept for the process lifetime, including activity recreation and all
    // windows. Only process death closes the writer; no compositor-held copy.
    private static android.os.ParcelFileDescriptor[] processLife;
    private static synchronized android.os.ParcelFileDescriptor lifeline() throws java.io.IOException {
        if(processLife==null)processLife=android.os.ParcelFileDescriptor.createPipe();
        return processLife[0];
    }
    private android.os.PowerManager.WakeLock portalWake;
    private boolean stopped;
    private String ref;
    private volatile int window;
    private boolean bound, attached;
    private volatile boolean destroyed;
    private TextView status;
    private SurfaceView view;
    private Surface surface;
    private IEmbeddedSession session;
    private final IEmbeddedWindowListener listener = new IEmbeddedWindowListener.Stub() {
        public boolean openUri(Intent intent) {
            java.util.concurrent.FutureTask<Boolean> task=new java.util.concurrent.FutureTask<>(() -> {
                if(destroyed || isFinishing())return false;
                try {startActivity(intent);return true;}catch(android.content.ActivityNotFoundException|SecurityException e){return false;}
            });
            runOnUiThread(task);
            try {return task.get(4,java.util.concurrent.TimeUnit.SECONDS);}
            catch(Exception e){task.cancel(false);return false;}
        }
        public void onWindowOpened(int id, int width, int height) {
            runOnUiThread(() -> {
                if (destroyed || window == id) return;
                if (window == 0) showWindow(id);
                else if (getIntent().getIntExtra(WINDOW,0)==0) startActivity(new Intent().setComponent(getComponentName())
                        .putExtra(WINDOW,id)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK));
            });
        }
        public void onInhibitChanged(boolean active) {
            runOnUiThread(() -> {
                if(destroyed)return;
                setInhibited(active);
            });
        }
        public void onWindowClosed(int id) {
            runOnUiThread(() -> { if (!destroyed && window == id) finish(); });
        }
    };
    private final ServiceConnection connection = new ServiceConnection() {
        public void onServiceConnected(ComponentName name, IBinder binder) {
            // Bridge verification may take time; never wait on the activity thread.
            new Thread(() -> {
                IEmbeddedSession opened=null;
                try {
                    opened=IEmbeddedHost.Stub.asInterface(binder).openSession(ref,listener,lifeline());
                    final IEmbeddedSession active=opened;
                    boolean needsLaunch=window==0;
                    runOnUiThread(() -> {
                        if (destroyed) {
                            try { active.unregisterListener(listener); } catch(Exception ignored){}
                            return;
                        }
                        session=active;
                        if (window!=0) { if(view==null)showWindow(window);else attachIfReady(); }
                    });
                    if (needsLaunch && !destroyed) {
                        JSONObject result=new JSONObject(active.launch());
                        if (!result.optBoolean("ok")) { message(result.optString("error","Application could not start"));return; }
                        message("Waiting for the application window…");
                        for(int i=0;i<30&&!destroyed&&window==0;i++) {
                            Thread.sleep(1000);
                            JSONObject state=new JSONObject(active.getLaunchStatus());
                            if (!state.optBoolean("ok")) { runOnUiThread(() -> {if(!destroyed)finish();});return; }
                        }
                        if(!destroyed&&window==0)message("Application is running, but no window has appeared.");
                    }
                    // Every window observes process exit, including secondary
                    // activities after the first window has closed.
                    while(!destroyed) {
                        Thread.sleep(1000);
                        JSONObject state=new JSONObject(active.getLaunchStatus());
                        if(!state.optBoolean("ok")) {runOnUiThread(() -> {if(!destroyed)finish();});break;}
                    }
                } catch(Exception e) { failure("Cannot start application",e); }
                finally {
                    if(destroyed&&opened!=null)try{opened.unregisterListener(listener);}catch(Exception ignored){}
                }
            },"flatpak-stub-launch").start();
        }
        public void onServiceDisconnected(ComponentName name) {
            setInhibited(false);
            session=null;attached=false;
            failure("Compositor disconnected",new IllegalStateException("The compositor stopped unexpectedly."));
            window=0;view=null;surface=null;
        }
    };
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        status=new TextView(this);status.setPadding(32,32,32,32);
        status.setText("Starting application…");setContentView(status);
        window=state!=null?state.getInt(WINDOW,0):getIntent().getIntExtra(WINDOW,0);
        int minimum=1;
        try {
            ActivityInfo info=getPackageManager().getActivityInfo(getComponentName(),PackageManager.GET_META_DATA);
            if(info.metaData!=null){ref=info.metaData.getString(HostContract.META_FLATPAK_REF);minimum=info.metaData.getInt(HostContract.META_MIN_INTERFACE,1);}
        } catch(Exception e){failure("Cannot read application reference",e);return;}
        if(ref==null||ref.trim().isEmpty()){message("This launcher has no application reference.");return;}
        if(minimum>HostContract.getInterfaceVersion()){message("Update the Linux host to launch this application.");return;}
        try {
            String[] requested = getPackageManager().getPackageInfo(getPackageName(), PackageManager.GET_PERMISSIONS).requestedPermissions;
            String permission = "org.matonos.permission.GAME_CONTROLLERS";
            if (window == 0 && requested != null) {
                java.util.List<String> declared = java.util.Arrays.asList(requested);
                // Ask at most once per stub. A denial is remembered so the
                // prompt is never repeated; the app still launches either way.
                // Do not gate on shouldShowRequestPermissionRationale(): it is
                // false before the first request, so the prompt would not appear.
                android.content.SharedPreferences answered = getSharedPreferences("flatpak_stub_permissions", MODE_PRIVATE);
                if (declared.contains(permission)
                        && checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED
                        && !answered.getBoolean("asked_" + permission, false)) {
                    answered.edit().putBoolean("asked_" + permission, true).apply();
                    requestPermissions(new String[]{permission}, 2900);
                    return;
                }
            }
        } catch (PackageManager.NameNotFoundException error) { failure("Cannot read permissions", error); return; }
        connectHost();
    }
    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(requestCode, permissions, grants);
        if ((requestCode == 2900 || requestCode == 2901) && !destroyed) connectHost();
    }
    private void connectHost() {
        Intent host=new Intent("org.matonos.compositor.EMBEDDED")
                .setClassName("org.matonos.compositor","org.matonos.compositor.CompositorService");
        try {
            startForegroundService(host);
            bound=bindService(host,connection,BIND_AUTO_CREATE);
            if(!bound)message("Cannot connect to the compositor.");
        } catch(Exception e){failure("Cannot connect to the compositor",e);}
    }
    // Runs on the activity thread, in the generated stub package's UID.
    private void setInhibited(boolean active) {
        if(active) {
            if(portalWake==null) {
                portalWake=getSystemService(android.os.PowerManager.class).newWakeLock(
                        android.os.PowerManager.PARTIAL_WAKE_LOCK,"MatonOS:JavaPortalInhibit");
                portalWake.setReferenceCounted(false);
            }
            // Generated stubs request WAKE_LOCK; an older stub without it keeps
            // only the screen-on flag instead of crashing.
            try {if(!portalWake.isHeld())portalWake.acquire();}
            catch(SecurityException e){android.util.Log.w("MatonStub","WAKE_LOCK not granted",e);}
            getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            if(portalWake!=null && portalWake.isHeld())portalWake.release();
            getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }
    private void showWindow(int id) {
        window=id;
        view=new SurfaceView(this);view.getHolder().addCallback(this);
        view.setFocusableInTouchMode(true);
        view.setOnTouchListener((v,event)->motion(event));
        view.setOnGenericMotionListener((v,event)->motion(event));
        setContentView(view);view.requestFocus();
    }
    private void message(String text) {
        runOnUiThread(()->{if(!destroyed&&window==0)status.setText(text);});
    }
    private void failure(String operation,Exception error) {
        android.util.Log.e("MatonFlatpakStub",operation+" for "+ref,error);
        String detail=error.getMessage()!=null?error.getMessage():error.getClass().getSimpleName();
        runOnUiThread(()->{
            if(destroyed)return;
            status.setText(operation+": "+detail);
            if(view!=null)setContentView(status);
        });
    }
    @Override protected void onStart() {
        super.onStart(); stopped=false; reportStopped();
    }
    @Override protected void onStop() {
        stopped=true; reportStopped(); super.onStop();
    }
    private void reportStopped() {
        if(session!=null && window!=0)try{session.setWindowStopped(window,stopped);}catch(Exception ignored){}
    }
    private void attachIfReady() {
        if(attached||session==null||surface==null||!surface.isValid()||window==0)return;
        try{session.attachWindow(window,surface,view.getWidth(),view.getHeight());attached=true;reportStopped();}
        catch(Exception e){failure("Cannot display application window",e);}
    }
    public void surfaceCreated(SurfaceHolder holder){surface=holder.getSurface();attachIfReady();}
    public void surfaceChanged(SurfaceHolder holder,int format,int width,int height){
        surface=holder.getSurface();attachIfReady();
        try{if(attached&&session!=null)session.resizeWindow(window,width,height);}
        catch(Exception e){failure("Cannot resize application window",e);}
    }
    public void surfaceDestroyed(SurfaceHolder holder){
        try{if(attached&&session!=null)session.detachWindow(window);}catch(Exception ignored){}
        attached=false;surface=null;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        if(session==null||window==0)return super.dispatchKeyEvent(event);
        try{session.keyEvent(window,event.getKeyCode(),event.getScanCode(),event.getAction(),event.getMetaState(),event.getEventTime()*1000000L);}
        catch(Exception e){failure("Cannot send keyboard input",e);}
        return true;
    }
    private boolean motion(MotionEvent event){
        if(session==null||window==0)return false;
        try{session.motionEvent(window,event.getX(),event.getY(),event.getAxisValue(MotionEvent.AXIS_VSCROLL),event.getAxisValue(MotionEvent.AXIS_HSCROLL),event.getActionMasked(),org.matonos.compositor.PointerInput.buttons(event.getActionMasked(),event.getButtonState(),event.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)),event.getEventTime()*1000000L);}
        catch(Exception e){failure("Cannot send pointer input",e);}
        return true;
    }
    @Override protected void onSaveInstanceState(Bundle state){state.putInt(WINDOW,window);super.onSaveInstanceState(state);}
    @Override protected void onDestroy(){
        destroyed=true;
        setInhibited(false);
        if(session!=null){
            try{if(attached)session.detachWindow(window);}catch(Exception ignored){}
            try{if(isFinishing()&&window!=0)session.closeWindow(window);}catch(Exception ignored){}
            try{session.unregisterListener(listener);}catch(Exception ignored){}
        }
        if(bound)unbindService(connection);
        super.onDestroy();
    }
}
