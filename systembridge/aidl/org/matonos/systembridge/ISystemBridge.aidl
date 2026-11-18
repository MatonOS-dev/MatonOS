package org.matonos.systembridge;

import android.os.ParcelFileDescriptor;
import org.matonos.systembridge.IMatonosListener;

/** MatonOS privileged bridge API. API version/hash are kept stable in-band. */
interface ISystemBridge {
    int getBridgeApiVersion();
    String getBridgeApiHash();
    String getBridgeStatus();
    String call(String target, String command, String jsonArgs);
    void subscribe(String target, String topic, IMatonosListener listener);
    void unsubscribe(String target, String topic, IMatonosListener listener);
    String stageUpdate(in String[] names, in ParcelFileDescriptor[] files);
    String finalizeUpdate();
    String getUpdateStatus();
}
