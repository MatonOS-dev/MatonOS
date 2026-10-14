package org.matonos.systembridge;

/** Pure policy mapping, shared by the bridge and the root-only debug harness. */
final class LinuxAppStorePolicy {
    private LinuxAppStorePolicy() {}
    static String restriction(String command) {
        if ("install".equals(command) || "install_app".equals(command)
                || "update_app".equals(command)) return "no_install_apps";
        if ("uninstall".equals(command) || "uninstall_app".equals(command))
            return "no_uninstall_apps";
        return null;
    }
}
