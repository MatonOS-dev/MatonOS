package org.matonos.systembridge;
import org.matonos.systembridge.IStoreListener;
import org.matonos.systembridge.LinuxAppInfo;
interface ILinuxAppStore {
    // Confirmation is bridge-owned; callbacks provide a caller-death lifeline.
    String installRepo(in byte[] flatpakrepoBytes);
    String removeRepo(String remoteName);
    String listRepos();
    String installApp(String appId, String remote);
    LinuxAppInfo[] checkUpdates();
    String updateApp(String appId);
    void uninstallApp(String appId, boolean allUsers);
    LinuxAppInfo[] listInstalled();
    void registerListener(IStoreListener listener);
}
