package org.matonos.systembridge;

import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.ServiceManager;
import android.util.Log;
import org.json.JSONObject;
import org.matonos.linuxhost.stubgen.StubGenerator;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Reconciles installed Flatpaks with signed Android launcher packages. */
final class FlatpakStubManager {
    private static final String TAG = "MatonFlatpakStubs";
    private final Context context;
    private final java.util.Map<Integer, FlatpakStubManager> users = new java.util.concurrent.ConcurrentHashMap<>();

    FlatpakStubManager forUser(int user) {
        if (user == context.getUserId()) return this;
        return users.computeIfAbsent(user, id -> new FlatpakStubManager(
                context.createContextAsUser(android.os.UserHandle.of(id), 0)));
    }

    int verifiedUid(String ref) throws Exception {
        PackageInfo info = context.getPackageManager().getPackageInfo(packageFor(ref), 0);
        if (info.applicationInfo == null || !ownsStub(info.applicationInfo.uid, ref))
            throw new SecurityException("Unverified Flatpak stub");
        return info.applicationInfo.uid;
    }

    private JSONObject installArgs(String ref) throws Exception {
        return new JSONObject().put("ref", ref).put("uid", verifiedUid(ref));
    }

    String uninstall(String ref,boolean deleteData) throws Exception {
        verifiedUid(ref); // Certificate and declared-ref checks precede PM removal.
        String pkg=packageFor(ref);
        context.getPackageManager().getPackageInstaller().uninstall(
                new android.content.pm.VersionedPackage(pkg,android.content.pm.PackageManager.VERSION_CODE_HIGHEST),
                deleteData?0:PackageManager.DELETE_KEEP_DATA,resultSender);
        return new JSONObject().put("ok",true).put("accepted",true).toString();
    }

    private String listArgs() throws Exception {
        return new JSONObject().put("userId", context.getUserId()).toString();
    }
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private ILinuxd subscribedDaemon;
    private boolean removalReceiverRegistered;
    private final ILinuxdListener listener = new ILinuxdListener.Stub() {
        @Override public void onEvent(String topic, String json) {
            try {
                JSONObject event = new JSONObject(json);
                if ("complete".equals(event.optString("phase"))) {
                    JSONObject result = event.optJSONObject("result");
                    if (result != null) {
                        // linuxd names the ref of installs a stub started itself.
                        String ref = result.optString("ref", "");
                        if (!ref.isEmpty() && result.has("uid")) {
                            FlatpakStubManager owner = forUser(android.os.UserHandle.getUserId(result.getInt("uid")));
                            if (result.optBoolean("ok")) worker.execute(() -> {
                                owner.showLauncher(ref);
                                owner.reconcileCompletedInstall(ref);
                            });
                            else worker.execute(() -> owner.removeFailedStub(ref));
                        }
                    }
                    refresh();
                }
            }
            catch (Exception e) { Log.w(TAG, "Invalid Flatpak completion event", e); }
        }
    };
    private final java.util.Map<Integer, Runnable> afterInstall = new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.Map<Integer, Runnable> installCleanup = new java.util.concurrent.ConcurrentHashMap<>();
    private final Set<String> pending = java.util.concurrent.ConcurrentHashMap.newKeySet();
    private final Set<String> permissionWork = java.util.concurrent.ConcurrentHashMap.newKeySet();
    /** Store operation IDs waiting for the stub's "install me", by ref. */
    private final IntentSender resultSender = new IntentSender((android.content.IIntentSender) new android.content.IIntentSender.Stub() {
        @Override public void send(int code, Intent intent, String resolvedType,
                android.os.IBinder whitelistToken, android.content.IIntentReceiver finishedReceiver,
                String requiredPermission, android.os.Bundle options) {
            worker.execute(() -> {
                if (intent == null) return;
                String pkg = intent.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME);
                pending.remove(pkg);
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                int sessionId = intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1);
                Runnable next = afterInstall.remove(sessionId);
                Runnable cleanup = installCleanup.remove(sessionId);
                if (status == PackageInstaller.STATUS_SUCCESS && next != null) next.run();
                if (cleanup != null) cleanup.run();
                if (status != PackageInstaller.STATUS_SUCCESS)
                    Log.e(TAG, "Stub package operation failed: " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
            });
        }
    });

    FlatpakStubManager(Context context) { this.context = context; }
    private final ILinuxdIdentity identity = new ILinuxdIdentity.Stub() {
        @Override public String query(String command, int uid, String packageName) {
            // linuxd is a system-UID service; never expose this callback to apps.
            if (android.os.Binder.getCallingUid() != android.os.Process.SYSTEM_UID)
                throw new SecurityException("Identity queries require linuxd");
            final long token = android.os.Binder.clearCallingIdentity();
            JSONObject result = new JSONObject();
            try {
                if (uid < 10000 || android.os.UserHandle.getAppId(uid) < 10000 ||
                        android.os.UserHandle.getAppId(uid) > 19999)
                    throw new SecurityException("Invalid application UID");
                FlatpakStubManager owner = forUser(android.os.UserHandle.getUserId(uid));
                PackageManager pm = owner.context.getPackageManager();
                int value = -1;
                {
                    if ("user-live".equals(command)) {
                        String pkg=BridgeCallerIdentity.solePackage(pm.getPackagesForUid(uid));
                        value=pkg!=null && FlatpakStubIdentity.isGeneratedStub(pkg) && owner.ownsStub(uid,owner.declaredRef(pkg))?1:0;
                    } else if ("live".equals(command)) {
                        // System-wide app-id ownership: preserve the deployment if
                        // any Android user still has a package at this app id.
                        value=0;
                        android.os.UserManager um=context.getSystemService(android.os.UserManager.class);
                        for(android.os.UserHandle user:um.getUserHandles(true)) {
                            PackageManager userPm=forUser(user.getIdentifier()).context.getPackageManager();
                            String[] packages=userPm.getPackagesForUid(android.os.UserHandle.getUid(user.getIdentifier(),android.os.UserHandle.getAppId(uid)));
                            if(packages!=null && packages.length>0){value=1;break;}
                        }
                    } else if ("verify".equals(command)) {
                        String pkg = BridgeCallerIdentity.solePackage(pm.getPackagesForUid(uid));
                        if (pkg != null && FlatpakStubIdentity.isGeneratedStub(pkg) && pkg.equals(packageName)) {
                            String ref = owner.declaredRef(pkg);
                            if (owner.ownsStub(uid, ref)) {
                                value = uid;
                                result.put("ref", ref).put("remote", owner.remoteFor(pkg));
                            }
                        }
                    }
                }
                result.put("value", value);
            } catch (Exception error) {
                try { result.put("value", -1); } catch (org.json.JSONException ignored) { }
                Log.w(TAG, "Cannot resolve linuxd identity", error);
            } finally {
                android.os.Binder.restoreCallingIdentity(token);
            }
            return result.toString();
        }
    };

    void start() {
        registerRemovalReceiver();
        worker.scheduleWithFixedDelay(() -> {
            reconcile();
            try {
                android.os.UserManager um=context.getSystemService(android.os.UserManager.class);
                for(android.os.UserHandle user:um.getUserHandles(true))
                    if(user.getIdentifier()!=context.getUserId())forUser(user.getIdentifier()).reconcile();
                daemon().call("reconcile_deployments","{}");
            } catch(Exception error){Log.w(TAG,"Cannot reconcile system-wide deployments",error);}
        },5,30,TimeUnit.SECONDS);
    }
    private final android.content.BroadcastReceiver removed=new android.content.BroadcastReceiver() {
        @Override public void onReceive(Context ignored,Intent intent) {
            if(intent.getBooleanExtra(Intent.EXTRA_REPLACING,false))return;
            if(!intent.getBooleanExtra(Intent.EXTRA_REMOVED_FOR_ALL_USERS,false))return;
            worker.execute(() -> {
                try{daemon().call("reconcile_deployments","{}");}
                catch(Exception error){Log.w(TAG,"Deployment removal deferred",error);}
            });
        }
    };
    private void registerRemovalReceiver() {
        android.content.IntentFilter filter=new android.content.IntentFilter(Intent.ACTION_PACKAGE_REMOVED);
        filter.addDataScheme("package");
        context.registerReceiverAsUser(removed,android.os.UserHandle.ALL,filter,null,null);
        removalReceiverRegistered=true;
    }
    void close() { if(removalReceiverRegistered)context.unregisterReceiver(removed);worker.shutdownNow(); for (FlatpakStubManager user : users.values()) user.close(); }
    void refresh() { worker.execute(this::reconcile); }

    String launch(String appId) throws Exception {
        if (appId == null || appId.length() > 255 || !appId.matches("[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid Flatpak application ID");
        ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
        if (daemon == null) throw new IllegalStateException("Flatpak service is unavailable");
        JSONObject list = new JSONObject(daemon.call("list_installed", listArgs()));
        if (!list.optBoolean("ok") || list.optBoolean("outputTruncated"))
            throw new IllegalStateException("Cannot read installed applications");
        for (String item : list.optString("output").split("\\r?\\n")) {
            String ref = item.trim();
            if (!ref.startsWith("app/")) ref = "app/" + ref;
            String[] parts = ref.split("/");
            if (parts.length != 4 || !parts[1].equals(appId)) continue;
            String pkg = packageFor(ref);
            // Readable stub names are predictable; verify the occupant is
            // really our signed stub before launching anything at that name.
            if (!isOurSignedStub(pkg))
                throw new IllegalStateException("The launcher entry for this application is not a MatonOS stub. Remove the conflicting package and retry.");
            Intent intent = context.getPackageManager().getLaunchIntentForPackage(pkg);
            if (intent == null) {
                refresh();
                throw new IllegalStateException("The launcher entry is still being created. Try again shortly.");
            }
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return new JSONObject().put("ok", true).toString();
        }
        throw new IllegalStateException("This application is no longer installed");
    }

    /** The package at a stub name must carry our stub signing certificate. */
    private boolean isOurSignedStub(String pkg) {
        try {
            PackageManager manager = context.getPackageManager();
            PackageInfo info = manager.getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.signingInfo == null) return false;
            byte[] expected = StubGenerator.getExistingSigningCertificate();
            for (android.content.pm.Signature signature : info.signingInfo.getApkContentsSigners())
                if (expected != null && java.security.MessageDigest.isEqual(signature.toByteArray(), expected))
                    return true;
            return false;
        } catch (Exception error) { return false; }
    }

    /**
     * Returns the declared commit information from a verified stub's manifest.
     * Reads APP_COMMIT_META, RUNTIME_REF_META, and RUNTIME_COMMIT_META from
     * the stub activity's meta-data. Returns JSON with appCommit, runtimeRef,
     * runtimeCommit (or an empty object if the stub has no commit info).
     */
    String stubCommits(int uid, String ref) throws Exception {
        if (!ownsStub(uid, ref)) throw new SecurityException("Unverified stub");
        String pkg = packageFor(ref);
        JSONObject result = new JSONObject();
        try {
            android.content.pm.ActivityInfo activity = context.getPackageManager().getActivityInfo(
                    new android.content.ComponentName(pkg, StubGenerator.HOST_ACTIVITY),
                    PackageManager.GET_META_DATA
                    | PackageManager.MATCH_DISABLED_COMPONENTS);
            if (activity != null && activity.metaData != null) {
                String appCommit = activity.metaData.getString(StubGenerator.APP_COMMIT_META);
                String runtimeRef = activity.metaData.getString(StubGenerator.RUNTIME_REF_META);
                String runtimeCommit = activity.metaData.getString(StubGenerator.RUNTIME_COMMIT_META);
                if (appCommit != null) result.put("appCommit", appCommit);
                if (runtimeRef != null) result.put("runtimeRef", runtimeRef);
                if (runtimeCommit != null) result.put("runtimeCommit", runtimeCommit);
            }
        } catch (Exception error) {
            Log.w(TAG, "Cannot read commit meta-data from stub " + pkg, error);
        }
        return result.toString();
    }

    private static String checkedPath(String path) {
        if (path == null || !path.startsWith("/data/app/") || path.contains("..") || path.contains("//"))
            throw new SecurityException("Invalid APK path");
        for (int i = 0; i < path.length(); i++) if (Character.isISOControl(path.charAt(i)))
            throw new SecurityException("Invalid APK path");
        return path;
    }

    private boolean signedByDevice(PackageInfo info) throws Exception {
        if (info == null || info.signingInfo == null || info.signingInfo.hasMultipleSigners()) return false;
        byte[] cert = StubGenerator.getExistingSigningCertificate();
        android.content.pm.Signature[] signers = info.signingInfo.getApkContentsSigners();
        return cert != null && signers.length == 1 && MessageDigest.isEqual(cert, signers[0].toByteArray());
    }

    private static String packageFor(String ref) throws Exception {
        return FlatpakStubIdentity.packageFor(ref);
    }

    boolean ownsStub(int uid,String ref) {
        if (uid < 0) return false;
        int user = android.os.UserHandle.getUserId(uid);
        if (user != context.getUserId()) return forUser(user).ownsStub(uid, ref);
        try {
            if(ref==null)return false;
            PackageManager manager=context.getPackageManager();
            String[] packages=manager.getPackagesForUid(uid);
            String pkg=BridgeCallerIdentity.solePackage(packages);
            // The caller must be exactly one generated stub package; never
            // attribute a shared UID and never trust a caller-supplied ref.
            if(pkg==null||!FlatpakStubIdentity.isGeneratedStub(pkg))return false;
            String declared=declaredRef(pkg);
            if(!FlatpakStubIdentity.refMatchesDeclared(ref,declared))return false;
            // The package name must also be the one this ref maps to, so a
            // mismatched package/ref pair can never borrow another identity.
            if(!pkg.equals(packageFor(declared)))return false;
            PackageInfo info=manager.getPackageInfo(pkg,PackageManager.GET_SIGNING_CERTIFICATES);
            if(info.applicationInfo==null||info.signingInfo==null)return false;
            android.content.pm.Signature[] signatures=info.signingInfo.getApkContentsSigners();
            byte[][] signers=new byte[signatures.length][];
            for(int i=0;i<signatures.length;i++)signers[i]=signatures[i].toByteArray();
            return FlatpakStubIdentity.matches(uid,info.applicationInfo.uid,packages,
                pkg,signers,StubGenerator.getExistingSigningCertificate());
        }catch(Exception error){Log.w(TAG,"Cannot verify generated launcher",error);return false;}
    }

    private String remoteFor(String pkg) {
        try {
            android.content.pm.ActivityInfo activity = context.getPackageManager().getActivityInfo(
                    new android.content.ComponentName(pkg, StubGenerator.HOST_ACTIVITY), PackageManager.GET_META_DATA
                    | PackageManager.MATCH_DISABLED_COMPONENTS);
            String remote = activity.metaData == null ? null : activity.metaData.getString(StubGenerator.REMOTE_META);
            if (remote == null || !remote.matches("[A-Za-z0-9._-]{1,128}"))
                throw new IllegalStateException("Stub manifest does not declare a valid Flatpak remote");
            return remote;
        } catch (PackageManager.NameNotFoundException error) {
            throw new IllegalStateException("Cannot read the installed stub manifest", error);
        }
    }

    /* Generated stubs ship with their launcher activity disabled, so the app
     * stays hidden until its Flatpak is installed. The enabled override
     * persists across later stub replacements. */
    private void showLauncher(String ref) {
        try {
            String pkg = packageFor(ref);
            PackageInfo info = context.getPackageManager().getPackageInfo(pkg, 0);
            if (info.applicationInfo == null || !ownsStub(info.applicationInfo.uid, ref)) return;
            context.getPackageManager().setComponentEnabledSetting(
                    new android.content.ComponentName(pkg, StubGenerator.HOST_ACTIVITY),
                    PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);
        } catch (Exception error) {
            Log.e(TAG, "Cannot show launcher after Flatpak install completed for " + ref, error);
        }
    }

    private void reconcileCompletedInstall(String ref) {
        try {
            String pkg = packageFor(ref);
            PackageInfo info = context.getPackageManager().getPackageInfo(pkg, PackageManager.GET_PERMISSIONS);
            int uid = info.applicationInfo == null ? -1 : info.applicationInfo.uid;
            if (uid < 10000 || !ownsStub(uid, ref)) return;
            JSONObject metadata = new JSONObject(daemon().call("metadata", installArgs(ref).toString()));
            if (!metadata.optBoolean("ok") || metadata.optBoolean("outputTruncated"))
                throw new IllegalStateException("Cannot read installed Flatpak permissions");
            Set<String> expected = new HashSet<>(StubGenerator.permissionsForMetadata(metadata.optString("output")));
            Set<String> declared = new HashSet<>();
            if (info.requestedPermissions != null) Collections.addAll(declared, info.requestedPermissions);
            Set<String> current = new HashSet<>();
            if (declared.contains(StubGenerator.GAME_CONTROLLERS)) current.add(StubGenerator.GAME_CONTROLLERS);
            if (pending.contains(pkg)) return;
            JSONObject entry = new JSONObject(daemon().call("desktop_entry", installArgs(ref).toString()));
            if (!entry.optBoolean("ok") || entry.optBoolean("outputTruncated"))
                throw new IllegalStateException("Cannot read installed Flatpak display name");
            String name = StubGenerator.desktopDisplayName(entry.optString("output"));
            String currentName = info.applicationInfo.loadLabel(context.getPackageManager()).toString();
            if (expected.equals(current) && name.equals(currentName)) return;
            String key = uid + ":" + ref;
            if (!permissionWork.add(key)) return;
            try { create(daemon(), ref, pkg, remoteFor(pkg), key); }
            catch (Exception error) {
                permissionWork.remove(key);
                throw error;
            }
        } catch (Exception error) {
            Log.e(TAG, "Cannot refresh stub after Flatpak install completed for " + ref, error);
        }
    }

    /** The Flatpak ref a generated stub declares in its own activity metadata. */
    private String declaredRef(String pkg) {
        try {
            android.content.pm.ActivityInfo activity=context.getPackageManager().getActivityInfo(
                new android.content.ComponentName(pkg,StubGenerator.HOST_ACTIVITY),
                PackageManager.GET_META_DATA
                    | PackageManager.MATCH_DISABLED_COMPONENTS);
            if(activity.metaData==null)return null;
            return activity.metaData.getString(StubGenerator.REF_META);
        }catch(Exception error){return null;}
    }

    boolean hasGameControllers(int uid, String ref) {
        int user = android.os.UserHandle.getUserId(uid);
        if (user != context.getUserId()) return forUser(user).hasGameControllers(uid, ref);
        try {
            String pkg = packageFor(ref);
            PackageInfo info = context.getPackageManager().getPackageInfo(pkg, 0);
            return info.applicationInfo != null && ownsStub(info.applicationInfo.uid, ref)
                    && context.getPackageManager().checkPermission(
                            StubGenerator.GAME_CONTROLLERS, pkg) == PackageManager.PERMISSION_GRANTED;
        } catch (Exception error) { return false; }
    }

    private void reconcile() {
        try {
            ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
            if (daemon == null) return;
            if (context.getUserId() == 0 && (subscribedDaemon == null || !subscribedDaemon.asBinder().equals(daemon.asBinder()))) {
                daemon.registerIdentity(identity);
                daemon.subscribe("progress", listener);
                subscribedDaemon = daemon;
                publishStubSigner(daemon);
            }
            JSONObject list = new JSONObject(daemon.call("list_installed", listArgs()));
            if (!list.optBoolean("ok") || list.optBoolean("outputTruncated")) return;
            Set<String> wanted = new HashSet<>();
            for (String item : list.optString("output").split("\\r?\\n")) {
                String ref = item.trim();
                if (ref.isEmpty()) continue;
                if (!ref.startsWith("app/")) ref = "app/" + ref;
                if (!ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+")) continue;
                String pkg = packageFor(ref);
                try { context.getPackageManager().getPackageInfo(pkg,0); }
                catch(PackageManager.NameNotFoundException absent){continue;}
                wanted.add(pkg);
                if (pending.contains(pkg)) continue;
                try {
                    PackageInfo installed = context.getPackageManager().getPackageInfo(pkg, 0);
                    if (installed.getLongVersionCode() >= StubGenerator.STUB_VERSION_CODE) {
                        showLauncher(ref); // covers a lost completion event
                        if (installed.applicationInfo != null && ref.split("/")[1].equals(
                                installed.applicationInfo.loadLabel(context.getPackageManager()).toString()))
                            reconcileCompletedInstall(ref); // finish a placeholder after a lost event
                        continue;
                    }
                }
                catch (PackageManager.NameNotFoundException expected) { }
                try { create(daemon, ref, pkg); }
                catch (Exception e) { Log.e(TAG, "Cannot create launcher for " + ref, e); }
            }
            for (PackageInfo pkg : context.getPackageManager().getInstalledPackages(0)) {
                // A hidden stub (launcher never enabled) is waiting for "install me";
                // only stubs whose Flatpak was installed and is now gone are removed.
                if (FlatpakStubIdentity.isGeneratedStub(pkg.packageName) && !wanted.contains(pkg.packageName) &&
                        launcherEnabled(pkg.packageName) && pending.add(pkg.packageName))
                    context.getPackageManager().getPackageInstaller().uninstall(pkg.packageName, resultSender);
            }
        } catch (Exception e) { Log.w(TAG, "Flatpak launcher reconciliation failed", e); }
    }

    /** A failed first install leaves a hidden stub with no Flatpak behind;
     * remove it. Failed updates keep theirs (the launcher is enabled). */
    private void removeFailedStub(String ref) {
        try {
            String pkg = packageFor(ref);
            ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
            if (daemon == null || launcherEnabled(pkg)) return;
            JSONObject list = new JSONObject(daemon.call("list_installed", listArgs()));
            if (!list.optBoolean("ok") || list.optBoolean("outputTruncated")) return;
            String bare = ref.substring("app/".length());
            for (String item : list.optString("output").split("\\r?\\n")) {
                String installed = item.trim();
                if (installed.equals(ref) || installed.equals(bare)) return;
            }
            context.getPackageManager().getPackageInfo(pkg, 0);
            if (pending.add(pkg)) context.getPackageManager().getPackageInstaller().uninstall(pkg, resultSender);
        } catch (PackageManager.NameNotFoundException gone) {
        } catch (Exception e) { Log.w(TAG, "Cannot remove stub after failed install of " + ref, e); }
    }

    private static byte[] normalizeIcon(byte[] bytes) throws java.io.IOException {
        Bitmap source = android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
        if (source == null) return bytes;
        int left=source.getWidth(), top=source.getHeight(), right=-1, bottom=-1;
        for (int y=0;y<source.getHeight();y++) for (int x=0;x<source.getWidth();x++) {
            if (Color.alpha(source.getPixel(x,y)) > 16) {
                left=Math.min(left,x);top=Math.min(top,y);right=Math.max(right,x);bottom=Math.max(bottom,y);
            }
        }
        if (right < left) { source.recycle(); return bytes; }
        Bitmap output=Bitmap.createBitmap(192,192,Bitmap.Config.ARGB_8888);
        float scale=(192f*66f/108f)/Math.max(right-left+1,bottom-top+1);
        float width=(right-left+1)*scale, height=(bottom-top+1)*scale;
        new Canvas(output).drawBitmap(source,
            new android.graphics.Rect(left,top,right+1,bottom+1),
            new android.graphics.RectF((192-width)/2,(192-height)/2,(192+width)/2,(192+height)/2),
            new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG));
        ByteArrayOutputStream encoded=new ByteArrayOutputStream();
        output.compress(Bitmap.CompressFormat.PNG,100,encoded);
        source.recycle();output.recycle();return encoded.toByteArray();
    }

    private void create(ILinuxd daemon, String ref, String pkg) throws Exception {
        create(daemon, ref, pkg, remoteFor(pkg), null);
    }

    private void create(ILinuxd daemon, String ref, String pkg, String remote, String workKey) throws Exception {
        JSONObject entry = new JSONObject(daemon.call("desktop_entry", installArgs(ref).toString()));
        if (!entry.optBoolean("ok")) throw new java.io.IOException(entry.optString("error", "Desktop entry unavailable"));
        File work = new File(context.getCacheDir(), "flatpak-stubs");
        if (!work.isDirectory() && !work.mkdirs()) throw new java.io.IOException("Cannot create stub scratch directory");
        File desktop = new File(work, pkg + ".desktop");
        File apk = new File(work, pkg + ".apk");
        try {
            try (FileOutputStream out = new FileOutputStream(desktop)) { out.write(entry.optString("output").getBytes(StandardCharsets.UTF_8)); }
            // Keep the store-provided artwork when an app has no bundled PNG.
            // Regenerating metadata/permissions must not erase a valid icon.
            byte[] iconBytes = installedIcon(pkg);
            if (iconBytes == null) iconBytes = fallbackIcon();
            JSONObject exported = new JSONObject(daemon.call("icon", installArgs(ref).toString()));
            if (exported.optBoolean("ok")) {
                byte[] candidate = android.util.Base64.decode(exported.optString("output"),android.util.Base64.DEFAULT);
                android.graphics.BitmapFactory.Options bounds = new android.graphics.BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                android.graphics.BitmapFactory.decodeByteArray(candidate,0,candidate.length,bounds);
                if (bounds.outWidth > 0 && bounds.outHeight > 0 && bounds.outWidth <= 1024 && bounds.outHeight <= 1024)
                    iconBytes = normalizeIcon(candidate);
            }
            JSONObject metadata = new JSONObject(daemon.call("metadata", installArgs(ref).toString()));
            if (!metadata.optBoolean("ok") || metadata.optBoolean("outputTruncated"))
                throw new java.io.IOException("Flatpak metadata unavailable");
            // Generate a plain stub without commit info (reconciliation doesn't
            // know the commits yet; those are added during Flatpak install).
            StubGenerator.generate(work, ref, remote, desktop, iconBytes,
                    StubGenerator.permissionsForMetadata(metadata.optString("output")), pkg, apk,
                    null, null);
            PackageInstaller installer = context.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(pkg);
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
            int id = installer.createSession(params);
            try (PackageInstaller.Session session = installer.openSession(id)) {
                try (FileInputStream input = new FileInputStream(apk); java.io.OutputStream output = session.openWrite("base.apk", 0, apk.length())) {
                    byte[] buffer = new byte[32768]; int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                    session.fsync(output);
                }
                if (workKey != null) installCleanup.put(id, () -> permissionWork.remove(workKey));
                afterInstall.put(id, () -> showLauncher(ref));
                pending.add(pkg); session.commit(resultSender);
            } catch (Exception e) { pending.remove(pkg); afterInstall.remove(id); installer.abandonSession(id); throw e; }
        } finally { desktop.delete(); apk.delete(); }
    }

    /* The store installs an identity-only stub first; once PackageManager
     * assigns its UID, stage and publish the Flatpak into that installation. */
    String installAsync(String ref, org.json.JSONObject args) throws Exception {
        if (ref == null || ref.length() > 255 || !ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid application reference");
        final String remote = args.optString("remote", "flathub");
        final String icon = args.optString("icon", null);
        final String operationId = args.optString("operationId", "");
        worker.execute(() -> {
            try { installStub(ref, icon, remote, operationId); }
            catch (Exception error) { Log.e(TAG, "Flatpak install failed for " + ref, error); }
        });
        return new JSONObject().put("ok", true).put("accepted", true).toString();
    }

    private void installStub(String ref, String icon, String remote, String operationId) throws Exception {
        String pkg = packageFor(ref);
        File work = new File(context.getCacheDir(), "flatpak-stubs");
        if (!work.isDirectory() && !work.mkdirs()) throw new java.io.IOException("Cannot create stub scratch directory");
        File desktop = new File(work, pkg + ".desktop");
        File apk = new File(work, pkg + ".apk");
        try {
            try (FileOutputStream out = new FileOutputStream(desktop)) {
                String appId = ref.split("/")[1];
                out.write(("[Desktop Entry]\nType=Application\nName=" + appId + "\n").getBytes(StandardCharsets.UTF_8));
            }
            byte[] iconBytes = iconFromBase64(icon);
            if (iconBytes == null) iconBytes = fallbackIcon();
            java.util.List<String> declaredPermissions = Collections.emptyList();
            try {
                PackageInfo existing = context.getPackageManager().getPackageInfo(pkg,
                        PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_PERMISSIONS);
                if (signedByDevice(existing) && existing.requestedPermissions != null &&
                        java.util.Arrays.asList(existing.requestedPermissions).contains(StubGenerator.GAME_CONTROLLERS))
                    declaredPermissions = Collections.singletonList(StubGenerator.GAME_CONTROLLERS);
            } catch (PackageManager.NameNotFoundException expected) { }
            StubGenerator.generate(work, ref, remote, desktop, iconBytes,
                    declaredPermissions, pkg, apk,
                    null, null);
            publishStubSigner(daemon());
            PackageInstaller installer = context.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(pkg);
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
            int id = installer.createSession(params);
            try (PackageInstaller.Session session = installer.openSession(id)) {
                try (FileInputStream input = new FileInputStream(apk); java.io.OutputStream output = session.openWrite("base.apk", 0, apk.length())) {
                    byte[] buffer = new byte[32768]; int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                    session.fsync(output);
                }
                /* The store then starts the stub's "install me" activity;
                 * the deploy runs under the UID PackageManager assigned. */
                pending.add(pkg);
                session.commit(resultSender);
            } catch (Exception e) { pending.remove(pkg); installer.abandonSession(id); throw e; }
        } finally { desktop.delete(); apk.delete(); }
    }

    private boolean launcherEnabled(String pkg) {
        try {
            return context.getPackageManager().getComponentEnabledSetting(
                    new android.content.ComponentName(pkg, StubGenerator.HOST_ACTIVITY))
                    == PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
        } catch (IllegalArgumentException gone) { return false; }
    }

    /** Legacy signer handoff; current linuxd uses the live identity provider. */
    private void publishStubSigner(ILinuxd daemon) {
        try {
            byte[] certificate = StubGenerator.getExistingSigningCertificate();
            if (certificate == null) return;
            StringBuilder hex = new StringBuilder(certificate.length * 2);
            for (byte b : certificate) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            JSONObject stored = new JSONObject(daemon.call("set_stub_signer",
                    new JSONObject().put("certificate", hex.toString()).toString()));
            if (!stored.optBoolean("ok")) Log.w(TAG, "linuxd refused the stub certificate: " + stored.optString("error"));
        } catch (Exception error) { Log.w(TAG, "Cannot hand the stub certificate to linuxd", error); }
    }

    private ILinuxd daemon() throws Exception {
        ILinuxd service = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
        if (service == null) throw new IllegalStateException("Flatpak service is unavailable");
        return service;
    }

    private static byte[] fallbackIcon() {
        Bitmap bitmap = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap); canvas.drawColor(Color.rgb(55, 95, 45));
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG); paint.setColor(Color.WHITE); paint.setTextSize(52); paint.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("L", 48, 67, paint);
        ByteArrayOutputStream icon = new ByteArrayOutputStream(); bitmap.compress(Bitmap.CompressFormat.PNG, 100, icon); bitmap.recycle();
        return icon.toByteArray();
    }

    private byte[] installedIcon(String pkg) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
            if (!signedByDevice(info) || info.applicationInfo == null) return null;
            try (java.util.zip.ZipFile apk = new java.util.zip.ZipFile(info.applicationInfo.sourceDir)) {
                java.util.zip.ZipEntry entry = apk.getEntry("res/drawable/foreground.png");
                if (entry == null || entry.getSize() <= 0 || entry.getSize() > 256 * 1024) return null;
                byte[] bytes;
                try (java.io.InputStream input = apk.getInputStream(entry)) {
                    bytes = input.readNBytes(256 * 1024 + 1);
                }
                if (bytes.length != entry.getSize()) return null;
                android.graphics.BitmapFactory.Options bounds = new android.graphics.BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
                if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 1024 || bounds.outHeight > 1024) return null;
                return bytes;
            }
        } catch (Exception error) { return null; }
    }

    private static byte[] iconFromBase64(String base64) {
        if (base64 == null || base64.isEmpty()) return null;
        try {
            byte[] candidate = android.util.Base64.decode(base64, android.util.Base64.DEFAULT);
            android.graphics.BitmapFactory.Options bounds = new android.graphics.BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            android.graphics.BitmapFactory.decodeByteArray(candidate, 0, candidate.length, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 1024 || bounds.outHeight > 1024) return null;
            return normalizeIcon(candidate);
        } catch (Exception error) { return null; }
    }
}
