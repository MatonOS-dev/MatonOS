package org.matonos.compositor;
import org.matonos.compositor.IEmbeddedWindowListener;

/** Launch methods are restricted to the verified stub UID. Rendering is local. */
interface IEmbeddedSession {
    String launch();
    String getLaunchStatus();
    void unregisterListener(IEmbeddedWindowListener listener);
}
