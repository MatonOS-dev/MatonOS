package org.matonos.systembridge;

/** Pure policy mapping, shared by the bridge and the root-only debug harness. */
final class LinuxAppStorePolicy {
    private LinuxAppStorePolicy() {}
    static void enforce(boolean permission, boolean allUsers, boolean adminOrSystemUser) {
        if (!permission) throw new SecurityException("MANAGE_LINUX_APPS required");
        if (allUsers && !adminOrSystemUser) throw new SecurityException("All-users uninstall requires an admin/system user");
    }
    static String restriction(String command) {
        if ("install".equals(command) || "install_app".equals(command)
                || "update_app".equals(command)) return "no_install_apps";
        if ("uninstall".equals(command) || "uninstall_app".equals(command))
            return "no_uninstall_apps";
        if ("install_repo".equals(command) || "remove_repo".equals(command))
            return "no_install_unknown_sources_globally";
        return null;
    }
}
