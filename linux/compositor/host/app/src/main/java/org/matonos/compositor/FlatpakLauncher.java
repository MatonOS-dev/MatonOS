package org.matonos.compositor;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.matonos.systembridge.ISystemBridge;

/** Verified launch requests; runtime display sockets belong to the app. */
final class FlatpakLauncher {
    private interface Request<T> { T run(ISystemBridge bridge) throws Exception; }
    static String launch(Context context,String ref,String dnsForwarder,int uid,int pid,ParcelFileDescriptor lifeline) throws Exception {
        return request(context,bridge->bridge.launchOwnedFlatpak(ref,null,null,dnsForwarder,uid,pid,lifeline));
    }
    static String status(Context context,String ref) throws Exception {return request(context,bridge->bridge.getFlatpakLaunchStatus(ref));}
    static boolean verifyStub(Context context,int uid,String ref) throws Exception {
        return request(context,bridge->bridge.isFlatpakStub(uid,ref));
    }
    static android.os.ParcelFileDescriptor[] createDnsSockets(Context context,int uid,String ref,String endpoint) throws Exception {
        if(endpoint==null||!endpoint.matches("127\\.(1[0-9]|[2-6][0-9]|7[0-3])\\.[0-9]{1,3}\\.[0-9]{1,3}:53"))
            throw new SecurityException("Invalid per-app DNS address");
        return request(context,bridge->bridge.createDnsForwarderSockets(endpoint.substring(0,endpoint.length()-3),uid,ref));
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
