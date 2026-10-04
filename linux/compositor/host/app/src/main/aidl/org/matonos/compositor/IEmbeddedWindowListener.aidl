package org.matonos.compositor;

import android.content.Intent;

interface IEmbeddedWindowListener {
    oneway void onWindowOpened(int id, int width, int height);
    oneway void onWindowClosed(int id);
    oneway void onInhibitChanged(boolean active);
    boolean openUri(in Intent intent);
}
