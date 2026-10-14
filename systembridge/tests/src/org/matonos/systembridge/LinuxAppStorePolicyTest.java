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
    @Test public void storePermissionIsRequiredEvenForAdmin() {
        for(boolean allUsers:new boolean[]{false,true})
            for(boolean admin:new boolean[]{false,true}) {
                try {LinuxAppStorePolicy.enforce(false,allUsers,admin);fail("missing permission accepted");}
                catch(SecurityException expected) {}
            }
    }
    @Test public void allUsersRequiresAdminOrSystemUser() {
        LinuxAppStorePolicy.enforce(true,false,false);
        LinuxAppStorePolicy.enforce(true,false,true);
        LinuxAppStorePolicy.enforce(true,true,true);
        try {LinuxAppStorePolicy.enforce(true,true,false);fail("non-admin accepted");}
        catch(SecurityException expected) {}
    }
}
