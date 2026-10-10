package org.matonos.compositor;

import java.io.IOException;
import java.util.List;

/**
 * One peer-to-peer D-Bus connection over a framed channel. Single-threaded:
 * {@link #serve()} blocks reading frames and dispatches them. Method calls go to
 * the handler, which returns the replies (and any signals) to send back; inbound
 * signals are handed straight to the handler.
 */
final class PeerConnection {
    interface Handler {
        List<PortalWire.Message> methodCall(PortalWire.Message call);
        void signal(PortalWire.Message message);
    }

    private final PortalChannel channel;
    private final Handler handler;
    private volatile boolean closed;

    PeerConnection(PortalChannel channel, Handler handler) {
        this.channel = channel;
        this.handler = handler;
    }

    /** Read and dispatch frames until the peer closes or {@link #close()} is called. */
    void serve() {
        try {
            while (!closed) {
                PortalChannel.Frame frame = channel.read();
                if (frame == null) break;
                try {
                    PortalWire.Message message = PortalWire.decode(frame.message, frame.fds);
                    if (message.type == 1) {
                        for (PortalWire.Message reply : handler.methodCall(message)) {
                            if ((message.flags & 1) == 0 || (reply.type != 2 && reply.type != 3)) send(reply);
                        }
                    } else if (message.type == 4) {
                        handler.signal(message);
                    }
                } finally {
                    // The channel owns the descriptors it received, so release
                    // them here, once per frame, whatever the outcome.
                    channel.closeDescriptors(frame.fds);
                }
            }
        } catch (IOException | RuntimeException error) {
            // Peer went away or sent something malformed; drop the connection.
        } finally {
            close();
        }
    }

    private void send(PortalWire.Message message) throws IOException {
        channel.write(PortalWire.encode(message), message.fds);
    }

    void close() {
        closed = true;
        try { channel.close(); } catch (IOException ignored) { }
    }
}
