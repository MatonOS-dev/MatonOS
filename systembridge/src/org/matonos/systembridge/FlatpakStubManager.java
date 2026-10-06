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
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor();
    private ILinuxd subscribedDaemon;
    private final ILinuxdListener listener = new ILinuxdListener.Stub() {
        @Override public void onEvent(String topic, String json) {
            try {
                JSONObject event = new JSONObject(json);
                if ("complete".equals(event.optString("phase"))) {
                    JSONObject result = event.optJSONObject("result");
                    if (result != null) {
                        String operationId = result.optString("operationId", "");
                        String ref = operationRefs.remove(operationId);
                        if (ref != null) {
                            installRefs.remove(ref);
                            if (result.optBoolean("ok")) worker.execute(() -> {
                                showLauncher(ref);
                                reconcileCompletedInstall(ref);
                            });
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
    private final Set<String> installRefs = java.util.concurrent.ConcurrentHashMap.newKeySet();
    /** Store operation IDs waiting for the stub's "install me", by ref. */
    private final java.util.concurrent.ConcurrentMap<String, String> storeOperations = new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.concurrent.ConcurrentMap<String, String> operationRefs = new java.util.concurrent.ConcurrentHashMap<>();
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
    void start() { worker.scheduleWithFixedDelay(this::reconcile, 5, 30, TimeUnit.SECONDS); }
    void close() { worker.shutdownNow(); }
    void refresh() { worker.execute(this::reconcile); }

    String launch(String appId) throws Exception {
        if (appId == null || appId.length() > 255 || !appId.matches("[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid Flatpak application ID");
        ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
        if (daemon == null) throw new IllegalStateException("Flatpak service is unavailable");
        JSONObject list = new JSONObject(daemon.call("list_installed", "{}"));
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

    /* "Install me" from the stub's unprivileged install activity, which the
     * store starts after creating the stub (or to resume an install a crash
     * or power cut interrupted). Does nothing when the Flatpak
     * is already installed for that UID (except making sure it is visible). */
    boolean installSelf(int uid, String ref) {
        if (!ownsStub(uid, ref)) throw new SecurityException("Unverified stub");
        String pkg;
        try { pkg = packageFor(ref); }
        catch (Exception error) { throw new IllegalArgumentException("Invalid Flatpak ref", error); }
        // A stub replacement in flight keeps the UID, so only a running deploy blocks.
        if (installRefs.contains(ref)) return false;
        try {
            JSONObject installed = new JSONObject(daemon().call("installed_for_uid",
                    new JSONObject().put("ref", ref).put("uid", uid).toString()));
            if (!installed.optBoolean("ok"))
                throw new IllegalStateException(installed.optString("error", "Cannot inspect Flatpak files"));
            if (Boolean.parseBoolean(installed.optString("output", "false"))) {
                showLauncher(ref);
                return true;
            }
        } catch (RuntimeException error) { throw error; }
        catch (Exception error) { throw new IllegalStateException("Cannot inspect Flatpak files", error); }
        if (!installRefs.add(ref)) return false;
        String remote = remoteFor(pkg);
        worker.execute(() -> {
            String operationId = storeOperations.remove(ref);
            try { deployStub(ref, pkg, remote, operationId == null || operationId.isEmpty() ? "stub-install-" + uid : operationId); }
            catch (Exception error) { Log.e(TAG, "Resumed Flatpak install failed for " + ref, error); }
        });
        return false;
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
            JSONObject metadata = new JSONObject(daemon().call("metadata", new JSONObject().put("ref", ref).toString()));
            if (!metadata.optBoolean("ok") || metadata.optBoolean("outputTruncated"))
                throw new IllegalStateException("Cannot read installed Flatpak permissions");
            Set<String> expected = new HashSet<>(StubGenerator.permissionsForMetadata(metadata.optString("output")));
            Set<String> declared = new HashSet<>();
            if (info.requestedPermissions != null) Collections.addAll(declared, info.requestedPermissions);
            Set<String> current = new HashSet<>();
            if (declared.contains(StubGenerator.GAME_CONTROLLERS)) current.add(StubGenerator.GAME_CONTROLLERS);
            if (expected.equals(current) || pending.contains(pkg)) return;
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

    boolean hasGameControllers(String ref) {
        try {
            String pkg = packageFor(ref);
            PackageInfo info = context.getPackageManager().getPackageInfo(pkg, 0);
            return info.applicationInfo != null && ownsStub(info.applicationInfo.uid, ref)
                    && context.getPackageManager().checkPermission(
                            StubGenerator.GAME_CONTROLLERS, pkg) == PackageManager.PERMISSION_GRANTED;
        } catch (Exception error) { return false; }
    }

    private void sweepLinuxData(ILinuxd daemon) throws Exception {
        JSONObject inventory=new JSONObject(daemon.call("linux_data_uids","{}"));
        if(!inventory.optBoolean("ok"))return;
        org.json.JSONArray uids=inventory.getJSONArray("uids");
        for(int i=0;i<uids.length();i++) {
            int uid=uids.getInt(i);
            // PM remains authoritative, including after user/package removal.
            // Preserve any reused UID: uncertainty must never destroy user data.
            String[] packages=context.getPackageManager().getPackagesForUid(uid);
            if(packages!=null && packages.length>0)continue;
            JSONObject reply=new JSONObject(daemon.call("delete_linux_data",new JSONObject().put("uid",uid).toString()));
            if(!reply.optBoolean("ok"))Log.w(TAG,"Linux data cleanup failed for uid "+uid);
        }
    }

    private void reconcile() {
        try {
            ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
            if (daemon == null) return;
            if (subscribedDaemon == null || !subscribedDaemon.asBinder().equals(daemon.asBinder())) {
                daemon.subscribe("progress", listener);
                subscribedDaemon = daemon;
            }
            sweepLinuxData(daemon);
            JSONObject list = new JSONObject(daemon.call("list_installed", "{}"));
            if (!list.optBoolean("ok") || list.optBoolean("outputTruncated")) return;
            Set<String> wanted = new HashSet<>();
            for (String item : list.optString("output").split("\\r?\\n")) {
                String ref = item.trim();
                if (ref.isEmpty()) continue;
                if (!ref.startsWith("app/")) ref = "app/" + ref;
                if (!ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+")) continue;
                String pkg = packageFor(ref);
                wanted.add(pkg);
                if (pending.contains(pkg)) continue;
                try {
                    if (context.getPackageManager().getPackageInfo(pkg, 0).getLongVersionCode() >= 10) {
                        showLauncher(ref); // covers a lost completion event
                        continue;
                    }
                }
                catch (PackageManager.NameNotFoundException expected) { }
                try { create(daemon, ref, pkg); }
                catch (Exception e) { Log.e(TAG, "Cannot create launcher for " + ref, e); }
            }
            for (PackageInfo pkg : context.getPackageManager().getInstalledPackages(0)) {
                if (FlatpakStubIdentity.isGeneratedStub(pkg.packageName) && !wanted.contains(pkg.packageName) && pending.add(pkg.packageName))
                    context.getPackageManager().getPackageInstaller().uninstall(pkg.packageName, resultSender);
            }
        } catch (Exception e) { Log.w(TAG, "Flatpak launcher reconciliation failed", e); }
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
        JSONObject entry = new JSONObject(daemon.call("desktop_entry", new JSONObject().put("ref", ref).toString()));
        if (!entry.optBoolean("ok")) throw new java.io.IOException(entry.optString("error", "Desktop entry unavailable"));
        File work = new File(context.getCacheDir(), "flatpak-stubs");
        if (!work.isDirectory() && !work.mkdirs()) throw new java.io.IOException("Cannot create stub scratch directory");
        File desktop = new File(work, pkg + ".desktop");
        File apk = new File(work, pkg + ".apk");
        try {
            try (FileOutputStream out = new FileOutputStream(desktop)) { out.write(entry.optString("output").getBytes(StandardCharsets.UTF_8)); }
            // Use a local fallback icon when the export contains only SVG assets.
            Bitmap bitmap = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bitmap); canvas.drawColor(Color.rgb(55, 95, 45));
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG); paint.setColor(Color.WHITE); paint.setTextSize(52); paint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText("L", 48, 67, paint);
            ByteArrayOutputStream icon = new ByteArrayOutputStream(); bitmap.compress(Bitmap.CompressFormat.PNG, 100, icon); bitmap.recycle();
            byte[] iconBytes = icon.toByteArray();
            JSONObject exported = new JSONObject(daemon.call("icon", new JSONObject().put("ref",ref).toString()));
            if (exported.optBoolean("ok")) {
                byte[] candidate = android.util.Base64.decode(exported.optString("output"),android.util.Base64.DEFAULT);
                android.graphics.BitmapFactory.Options bounds = new android.graphics.BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                android.graphics.BitmapFactory.decodeByteArray(candidate,0,candidate.length,bounds);
                if (bounds.outWidth > 0 && bounds.outHeight > 0 && bounds.outWidth <= 1024 && bounds.outHeight <= 1024)
                    iconBytes = normalizeIcon(candidate);
            }
            JSONObject metadata = new JSONObject(daemon.call("metadata", new JSONObject().put("ref", ref).toString()));
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

    private JSONObject stage(String ref, String remote) throws Exception {
        JSONObject request = new JSONObject().put("ref", ref).put("remote", remote);
        JSONObject staged = new JSONObject(daemon().call("stage", request.toString()));
        if (!staged.optBoolean("ok"))
            throw new java.io.IOException(staged.optString("error", "Flatpak staging failed"));
        return staged;
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
                storeOperations.put(ref, operationId == null ? "" : operationId);
                pending.add(pkg);
                session.commit(resultSender);
            } catch (Exception e) { pending.remove(pkg); storeOperations.remove(ref); installer.abandonSession(id); throw e; }
        } finally { desktop.delete(); apk.delete(); }
    }

    private void deployStub(String ref, String pkg, String remote, String operationId) {
        try {
            JSONObject staged = stage(ref, remote);
            PackageInfo info = context.getPackageManager().getPackageInfo(pkg, 0);
            int stubUid = info.applicationInfo.uid;
            int runtimeUid = runtimeAppUid(android.os.UserHandle.getUserId(stubUid));
            if (stubUid < 10000 || runtimeUid < 10000) throw new java.io.IOException("Missing stub or runtime UID");
            JSONObject request = new JSONObject()
                    .put("ref", ref)
                    .put("operationId", operationId == null ? "" : operationId)
                    .put("appCommit", staged.getString("appCommit"))
                    .put("runtimeRef", staged.getString("runtimeRef"))
                    .put("runtimeCommit", staged.getString("runtimeCommit"))
                    .put("remote", remote)
                    .put("uid", stubUid)
                    .put("runtimeUid", runtimeUid);
            operationRefs.put(operationId, ref);
            JSONObject published = new JSONObject(daemon().call("install", request.toString()));
            if (!published.optBoolean("ok") || !published.optBoolean("accepted"))
                throw new java.io.IOException(published.optString("error", "Flatpak install was rejected"));
        } catch (Exception error) {
            operationRefs.remove(operationId);
            installRefs.remove(ref);
            Log.e(TAG, "Flatpak deploy failed for " + ref, error);
            throw new IllegalStateException("Flatpak deploy failed for " + ref, error);
        }
    }

    private ILinuxd daemon() throws Exception {
        ILinuxd service = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
        if (service == null) throw new IllegalStateException("Flatpak service is unavailable");
        return service;
    }

    private int runtimeAppUid(int userId) {
        try {
            return context.getPackageManager().getApplicationInfoAsUser("org.matonos.linuxruntimes", 0, userId).uid;
        } catch (Exception error) { return -1; }
    }

    private static byte[] fallbackIcon() {
        Bitmap bitmap = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap); canvas.drawColor(Color.rgb(55, 95, 45));
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG); paint.setColor(Color.WHITE); paint.setTextSize(52); paint.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("L", 48, 67, paint);
        ByteArrayOutputStream icon = new ByteArrayOutputStream(); bitmap.compress(Bitmap.CompressFormat.PNG, 100, icon); bitmap.recycle();
        return icon.toByteArray();
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
