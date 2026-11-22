package org.matonos.caption;

import org.matonos.caption.ICaptionHost;

import android.os.Bundle;
import android.os.IBinder;

interface ICaptionProvider {
    const int VERSION = 1;
    int getVersion();
    void attach(int taskId, int displayId, in IBinder hostToken, int widthPx, int heightPx, in Bundle taskState, ICaptionHost host);
    void update(int taskId, int widthPx, int heightPx, in Bundle taskState);
    void detach(int taskId);
}
