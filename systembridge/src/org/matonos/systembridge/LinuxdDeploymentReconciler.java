package org.matonos.systembridge;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.util.Log;
import org.json.JSONObject;
import java.security.MessageDigest;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Keeps linuxd deployment ownership current and answers its live PackageManager identity queries. */
final class LinuxdDeploymentReconciler {
    private static final String TAG = "MatonLinuxdIdentity";
    private final Context context;
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private boolean removalReceiverRegistered;
    private ILinuxd registeredDaemon;

    LinuxdDeploymentReconciler(Context context) { this.context = context; }

    private LinuxdDeploymentReconciler forUser(int user) {
        return user == context.getUserId() ? this :
                new LinuxdDeploymentReconciler(context.createContextAsUser(UserHandle.of(user), 0));
    }

    private final ILinuxdIdentity identity = new ILinuxdIdentity.Stub() {
        @Override public String query(String command, int uid, String packageName) {
            if (android.os.Binder.getCallingUid() != android.os.Process.SYSTEM_UID)
                throw new SecurityException("Identity queries require linuxd");
            long token = android.os.Binder.clearCallingIdentity();
            JSONObject result = new JSONObject();
            try {
                int appId = UserHandle.getAppId(uid);
                if (uid < 10000 || appId < 10000 || appId > 19999)
                    throw new SecurityException("Invalid application UID");
                LinuxdDeploymentReconciler owner = forUser(UserHandle.getUserId(uid));
                PackageManager pm = owner.context.getPackageManager();
                String pkg = BridgeCallerIdentity.solePackage(pm.getPackagesForUid(uid));
                int value = -1;
                if ("user-live".equals(command)) {
                    value = pkg != null && FlatpakStubIdentity.isGeneratedStub(pkg) &&
                            owner.ownsStub(uid, owner.declaredRef(pkg)) ? 1 : 0;
                } else if ("live".equals(command)) {
                    value = 0;
                    android.os.UserManager users = context.getSystemService(android.os.UserManager.class);
                    if (users != null) for (UserHandle user : users.getUserHandles(true)) {
                        String[] packages = forUser(user.getIdentifier()).context.getPackageManager()
                                .getPackagesForUid(UserHandle.getUid(user.getIdentifier(), appId));
                        if (packages != null && packages.length != 0) { value = 1; break; }
                    }
                } else if ("verify".equals(command) && pkg != null && pkg.equals(packageName) &&
                        FlatpakStubIdentity.isGeneratedStub(pkg)) {
                    String ref = owner.declaredRef(pkg);
                    if (owner.ownsStub(uid, ref)) {
                        value = uid;
                        result.put("ref", ref).put("remote", owner.remoteFor(pkg));
                    }
                }
                result.put("value", value);
            } catch (Exception error) {
                try { result.put("value", -1); } catch (org.json.JSONException ignored) { }
                Log.w(TAG, "Cannot resolve linuxd identity", error);
            } finally { android.os.Binder.restoreCallingIdentity(token); }
            return result.toString();
        }
    };

    void start() {
        android.content.IntentFilter filter = new android.content.IntentFilter(Intent.ACTION_PACKAGE_REMOVED);
        filter.addDataScheme("package");
        context.registerReceiverAsUser(removed, UserHandle.ALL, filter, null, null);
        removalReceiverRegistered = true;
        worker.scheduleWithFixedDelay(this::reconcileDeployments, 5, 30, TimeUnit.SECONDS);
    }

    private final android.content.BroadcastReceiver removed = new android.content.BroadcastReceiver() {
        @Override public void onReceive(Context ignored, Intent intent) {
            if (intent.getBooleanExtra(Intent.EXTRA_REPLACING, false) ||
                    !intent.getBooleanExtra(Intent.EXTRA_REMOVED_FOR_ALL_USERS, false)) return;
            worker.execute(LinuxdDeploymentReconciler.this::reconcileDeployments);
        }
    };

    private void reconcileDeployments() {
        try {
            ILinuxd daemon = ILinuxd.Stub.asInterface(
                    ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
            if (daemon == null) return;
            if (context.getUserId() == 0 && (registeredDaemon == null ||
                    !registeredDaemon.asBinder().equals(daemon.asBinder()))) {
                daemon.registerIdentity(identity);
                registeredDaemon = daemon;
                daemon.asBinder().linkToDeath(() -> worker.execute(this::reconcileDeployments), 0);
            }
            daemon.call("reconcile_deployments", "{}");
        } catch (Exception error) { Log.w(TAG, "Cannot reconcile linuxd deployments", error); }
    }

    void close() {
        if (removalReceiverRegistered) context.unregisterReceiver(removed);
        worker.shutdownNow();
    }

    private boolean ownsStub(int uid, String ref) {
        if (uid < 0 || ref == null) return false;
        try {
            PackageManager pm = context.getPackageManager();
            String pkg = BridgeCallerIdentity.solePackage(pm.getPackagesForUid(uid));
            if (pkg == null || !FlatpakStubIdentity.isGeneratedStub(pkg) ||
                    !FlatpakStubIdentity.refMatchesDeclared(ref, declaredRef(pkg)) ||
                    !pkg.equals(FlatpakStubIdentity.packageFor(ref))) return false;
            PackageInfo info = pm.getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.applicationInfo == null || info.applicationInfo.uid != uid || info.signingInfo == null ||
                    info.signingInfo.hasMultipleSigners()) return false;
            android.content.pm.Signature[] signers = info.signingInfo.getApkContentsSigners();
            byte[] expected = recordedStubCertificate(pkg);
            return signers.length == 1 && expected != null &&
                    MessageDigest.isEqual(expected, signers[0].toByteArray());
        } catch (Exception error) { return false; }
    }

    private String declaredRef(String pkg) {
        try {
            android.content.pm.ActivityInfo activity = context.getPackageManager().getActivityInfo(
                    new android.content.ComponentName(pkg, "org.matonos.linux.stub.StubActivity"),
                    PackageManager.GET_META_DATA | PackageManager.MATCH_DISABLED_COMPONENTS);
            return activity.metaData == null ? null : activity.metaData.getString("org.matonos.linuxhost.FLATPAK_REF");
        } catch (Exception ignored) { return null; }
    }

    private String remoteFor(String pkg) throws Exception {
        android.content.pm.ActivityInfo activity = context.getPackageManager().getActivityInfo(
                new android.content.ComponentName(pkg, "org.matonos.linux.stub.StubActivity"),
                PackageManager.GET_META_DATA | PackageManager.MATCH_DISABLED_COMPONENTS);
        String remote = activity.metaData == null ? null :
                activity.metaData.getString("org.matonos.linuxhost.FLATPAK_REMOTE");
        if (remote == null || !remote.matches("[A-Za-z0-9._-]{1,128}"))
            throw new SecurityException("Invalid stub remote metadata");
        return remote;
    }

    private byte[] recordedStubCertificate(String pkg) throws Exception {
        String ref = declaredRef(pkg);
        if (ref == null || !pkg.equals(FlatpakStubIdentity.packageFor(ref))) return null;
        ILinuxd daemon = ILinuxd.Stub.asInterface(
                ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
        if (daemon == null) return null;
        JSONObject response = new JSONObject(daemon.call("stub_identity", new JSONObject().put("package", pkg).toString()));
        if (!response.optBoolean("ok") || !ref.equals(response.optString("ref"))) return null;
        String hex = response.optString("certSha256", "");
        if (!hex.matches("[0-9a-f]{64}")) return null;
        byte[] digest = new byte[32];
        for (int i = 0; i < digest.length; i++) digest[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
        PackageInfo info = context.getPackageManager().getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
        if (info.signingInfo == null || info.signingInfo.hasMultipleSigners()) return null;
        android.content.pm.Signature[] signatures = info.signingInfo.getApkContentsSigners();
        if (signatures.length != 1 || !MessageDigest.isEqual(digest,
                MessageDigest.getInstance("SHA-256").digest(signatures[0].toByteArray()))) return null;
        return signatures[0].toByteArray();
    }
}
