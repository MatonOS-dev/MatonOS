package org.matonos.compositor;

import android.view.Surface;

interface ICompositor {
    void launchDemo();
    void attachWindow(int windowId, in Surface surface, int width, int height);
    void detachWindow(int windowId);
    void keyEvent(int windowId, int keycode, int scanCode, int action, int metaState, long eventTimeNanos);
    void motionEvent(int windowId, float x, float y, float verticalScroll, float horizontalScroll, int action, int buttons, long eventTimeNanos);
    void resizeWindow(int windowId, int width, int height);
    String launchFlatpak(String ref);
    void closeWindow(int id);
    String getLaunchStatus(String ref);
    boolean isInhibited();
}
