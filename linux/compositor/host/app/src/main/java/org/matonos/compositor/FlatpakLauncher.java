package org.matonos.compositor;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.matonos.systembridge.ISystemBridge;

/** The bridge gets a directory capability for Wayland sockets only. */
final class FlatpakLauncher {
    private interface Request<T> { T run(ISystemBridge bridge) throws Exception; }
    static String launch(Context context,String ref) throws Exception {
        return launch(context,ref,new java.io.File(context.getFilesDir(),"wayland"));
    }
    static String launch(Context context,String ref,java.io.File runtime) throws Exception {
        return request(context,bridge->{
            java.io.FileDescriptor directory=Os.open(runtime.getAbsolutePath(),OsConstants.O_RDONLY|OsConstants.O_CLOEXEC,0);
            try(ParcelFileDescriptor capability=ParcelFileDescriptor.dup(directory)){
                return bridge.launchFlatpak(ref,capability);
            }finally{Os.close(directory);}
        });
    }
    static String status(Context context,String ref) throws Exception {return request(context,bridge->bridge.getFlatpakLaunchStatus(ref));}
    static boolean verifyStub(Context context,int uid,String ref) throws Exception {
        return request(context,bridge->bridge.isFlatpakStub(uid,ref));
    }
    private static <T> T request(Context context, Request<T> request) throws Exception {
        CountDownLatch connected = new CountDownLatch(1);
        ISystemBridge[] bridge = new ISystemBridge[1];
        ServiceConnection connection = new ServiceConnection() {
            public void onServiceConnected(ComponentName name, IBinder binder) {
                bridge[0] = ISystemBridge.Stub.asInterface(binder); connected.countDown();
            }
            public void onServiceDisconnected(ComponentName name) { bridge[0] = null; }
            public void onNullBinding(ComponentName name) { connected.countDown(); }
        };
        Intent intent = new Intent().setClassName("org.matonos.systembridge", "org.matonos.systembridge.SystemBridgeService");
        if (!context.bindService(intent, connection, Context.BIND_AUTO_CREATE))
            throw new IllegalStateException("Cannot connect to the system bridge");
        try {
            if (!connected.await(10,TimeUnit.SECONDS) || bridge[0] == null)
                throw new IllegalStateException("System bridge connection timed out");
            return request.run(bridge[0]);
        } finally { context.unbindService(connection); }
    }
}
