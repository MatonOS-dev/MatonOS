package org.matonos.compositor;
import android.view.Surface;
import org.matonos.compositor.IEmbeddedWindowListener;

/** Session methods are restricted to the stub UID and its owned windows. */
interface IEmbeddedSession {
    String launch();
    String getLaunchStatus();
    void unregisterListener(IEmbeddedWindowListener listener);
    void attachWindow(int id, in Surface surface, int width, int height);
    void detachWindow(int id);
    void resizeWindow(int id, int width, int height);
    void closeWindow(int id);
    void keyEvent(int id, int key, int scan, int action, int meta, long time);
    void motionEvent(int id, float x, float y, float vs, float hs, int action, int buttons, long time);
}
