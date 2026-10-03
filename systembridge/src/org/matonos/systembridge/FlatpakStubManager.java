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
            try { if ("complete".equals(new JSONObject(json).optString("phase"))) refresh(); }
            catch (Exception e) { Log.w(TAG, "Invalid Flatpak completion event", e); }
        }
    };
    private final Set<String> pending = new HashSet<>();
    private final IntentSender resultSender = new IntentSender((android.content.IIntentSender) new android.content.IIntentSender.Stub() {
        @Override public void send(int code, Intent intent, String resolvedType,
                android.os.IBinder whitelistToken, android.content.IIntentReceiver finishedReceiver,
                String requiredPermission, android.os.Bundle options) {
            worker.execute(() -> {
                if (intent == null) return;
                String pkg = intent.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME);
                pending.remove(pkg);
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
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

    private static String packageFor(String ref) throws Exception {
        return FlatpakStubIdentity.packageFor(ref);
    }

    boolean ownsStub(int uid,String ref) {
        try {
            String pkg=packageFor(ref);
            PackageManager manager=context.getPackageManager();
            PackageInfo info=manager.getPackageInfo(pkg,PackageManager.GET_SIGNING_CERTIFICATES);
            if(info.applicationInfo==null||info.signingInfo==null)return false;
            android.content.pm.Signature[] signatures=info.signingInfo.getApkContentsSigners();
            byte[][] signers=new byte[signatures.length][];
            for(int i=0;i<signatures.length;i++)signers[i]=signatures[i].toByteArray();
            return FlatpakStubIdentity.matches(uid,info.applicationInfo.uid,manager.getPackagesForUid(uid),
                pkg,signers,StubGenerator.getExistingSigningCertificate());
        }catch(Exception error){Log.w(TAG,"Cannot verify generated launcher",error);return false;}
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

    private void reconcile() {
        try {
            ILinuxd daemon = ILinuxd.Stub.asInterface(ServiceManager.checkService("org.matonos.systembridge.ILinuxd/default"));
            if (daemon == null) return;
            if (subscribedDaemon == null || !subscribedDaemon.asBinder().equals(daemon.asBinder())) {
                daemon.subscribe("progress", listener);
                subscribedDaemon = daemon;
            }
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
                try { if (context.getPackageManager().getPackageInfo(pkg, 0).getLongVersionCode() >= 10) continue; }
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
            StubGenerator.generate(work, ref, desktop, iconBytes,
                    StubGenerator.permissionsForMetadata(metadata.optString("output")), pkg, apk);
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
                pending.add(pkg); session.commit(resultSender);
            } catch (Exception e) { pending.remove(pkg); installer.abandonSession(id); throw e; }
        } finally { desktop.delete(); apk.delete(); }
    }
}
