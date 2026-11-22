package org.matonos.caption;

import org.matonos.caption.ICaptionSettingsHost;

import android.os.IBinder;

interface ICaptionSettings {
    const int VERSION = 1;
    int getVersion();
    void open(in IBinder hostToken, int displayId, int widthPx, int heightPx, ICaptionSettingsHost host);
    void resize(int widthPx, int heightPx);
    void close();
}
