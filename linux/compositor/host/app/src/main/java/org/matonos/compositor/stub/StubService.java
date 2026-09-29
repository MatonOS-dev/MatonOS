package org.matonos.compositor.stub;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** Reserved host-library service entry for later portal and D-Bus integration. */
public final class StubService extends Service {
    @Override public IBinder onBind(Intent intent) { return null; }
}
