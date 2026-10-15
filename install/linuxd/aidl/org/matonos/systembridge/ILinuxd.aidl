package org.matonos.systembridge;

import org.matonos.systembridge.ILinuxdListener;

interface ILinuxd {
    /** Flatpak installation and deployment management. */
    String call(String command, String jsonArgs);
    void subscribe(String topic, ILinuxdListener listener);
    void unsubscribe(String topic, ILinuxdListener listener);
}
