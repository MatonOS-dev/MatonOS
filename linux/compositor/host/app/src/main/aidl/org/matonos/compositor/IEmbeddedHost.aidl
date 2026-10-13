package org.matonos.compositor;
import org.matonos.compositor.IEmbeddedSession;
import org.matonos.compositor.IEmbeddedWindowListener;

/** Only a verified generated stub may open its own application's session. */
interface IEmbeddedHost {
    IEmbeddedSession openSession(String ref, IEmbeddedWindowListener listener, in android.os.ParcelFileDescriptor lifeline, String dnsForwarder);
}
