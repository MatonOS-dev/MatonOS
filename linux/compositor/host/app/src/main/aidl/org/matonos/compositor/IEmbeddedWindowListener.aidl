package org.matonos.compositor;

oneway interface IEmbeddedWindowListener {
    void onWindowOpened(int id, int width, int height);
    void onWindowClosed(int id);
    void onInhibitChanged(boolean active);
}
