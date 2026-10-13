package org.matonos.compositor;
import org.matonos.compositor.IEmbeddedWindowListener;

/** Launch methods are restricted to the verified stub UID. Rendering is local. */
interface IEmbeddedSession {
    // Returns a validated launch spec; never spawns in the host or linuxd.
    String launch();
    void unregisterListener(IEmbeddedWindowListener listener);
}
