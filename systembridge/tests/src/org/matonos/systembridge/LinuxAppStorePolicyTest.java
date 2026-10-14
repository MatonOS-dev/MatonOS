package org.matonos.systembridge;
import static org.junit.Assert.*;
import org.junit.Test;
public final class LinuxAppStorePolicyTest {
    @Test public void mutationsUsePlatformRestrictions() {
        for (String command : new String[]{"install", "install_app", "update_app"})
            assertEquals("no_install_apps", LinuxAppStorePolicy.restriction(command));
        for (String command : new String[]{"uninstall", "uninstall_app"})
            assertEquals("no_uninstall_apps", LinuxAppStorePolicy.restriction(command));
    }
    @Test public void queriesDoNotRequireMutationPermission() {
        assertNull(LinuxAppStorePolicy.restriction("list_installed"));
        assertNull(LinuxAppStorePolicy.restriction("update_check"));
        assertNull(LinuxAppStorePolicy.restriction("list_remotes"));
    }
}
