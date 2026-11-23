package org.matonos.settings.gapps;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.net.Uri;
import android.provider.Settings;

import expo.modules.kotlin.Promise;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipFile;
import org.tukaani.xz.XZInputStream;

/** User-supplied GApps validation and update installation. */
public final class GappsInstaller {
    private static final int PICK_REQUEST = 7412;
    private static final String GOOGLE_ROOT = "F0FD6C5B410F25CB25C3B53346C8972FAE30F8EE7411DF910480AD6B2D60DB83";
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor();

    private GappsInstaller() {}

    public static void pick(Activity activity, Promise promise) {
        GappsPickerActivity.promise = promise;
        Intent intent = new Intent(activity, GappsPickerActivity.class);
        activity.startActivity(intent);
    }

    static void launchPicker(Activity activity) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                "application/vnd.android.package-archive", "application/zip", "application/x-zip-compressed",
                "application/x-xz", "application/octet-stream"});
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        activity.startActivityForResult(intent, PICK_REQUEST);
    }

    static void handlePickerResult(Activity activity, int resultCode, Intent data) {
        Promise promise = GappsPickerActivity.promise;
        if (promise == null) return;
        if (resultCode != Activity.RESULT_OK || data == null) {
            GappsPickerActivity.promise = null;
            promise.resolve(map("cancelled", true, "message", ""));
            activity.finish();
            return;
        }
        ArrayList<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) {
            for (int index = 0; index < data.getClipData().getItemCount(); index++) {
                uris.add(data.getClipData().getItemAt(index).getUri());
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        Promise current = promise;
        GappsPickerActivity.promise = null;
        WORKER.execute(() -> {
            try {
                List<File> apks = collectApks(activity, uris);
                if (apks.isEmpty()) throw new IllegalArgumentException("No supported Google APKs were found in the selected files.");
                List<Apk> validated = validate(activity, apks);
                installNext(activity, validated, 0, current, new ArrayList<String>());
            } catch (Exception error) {
                current.resolve(map("cancelled", false, "ok", false, "message", safeMessage(error)));
                activity.runOnUiThread(activity::finish);
            }
        });
    }

    private static List<File> collectApks(Context context, List<Uri> uris) throws Exception {
        File root = new File(context.getCacheDir(), "user-gapps-" + System.currentTimeMillis());
        if (!root.mkdirs() && !root.isDirectory()) throw new IllegalStateException("Could not prepare temporary storage.");
        List<File> output = new ArrayList<>();
        int ordinal = 0;
        for (Uri uri : uris) {
            String name = "input-" + (ordinal++);
            File input = new File(root, name);
            try (InputStream source = new BufferedInputStream(context.getContentResolver().openInputStream(uri));
                 OutputStream target = new BufferedOutputStream(new FileOutputStream(input))) {
                if (source == null) throw new IllegalArgumentException("Could not read a selected file.");
                copy(source, target);
            }
            String lower = uri.getLastPathSegment() == null ? "" : uri.getLastPathSegment().toLowerCase(Locale.ROOT);
            if (lower.endsWith(".apk")) output.add(input);
            else if (lower.endsWith(".tar.xz") || isXz(input)) {
                extractTarXz(input, root, output);
            } else if (isZip(input)) {
                extractZip(input, root, output);
            } else {
                throw new IllegalArgumentException("Select APK files or a supported GApps zip.");
            }
        }
        return output;
    }

    private static void extractZip(File zip, File root, List<File> output) throws Exception {
        try (ZipInputStream input = new ZipInputStream(new BufferedInputStream(new FileInputStream(zip)))) {
            ZipEntry entry;
            int count = 0;
            byte[] buffer = new byte[32768];
            while ((entry = input.getNextEntry()) != null) {
                String name = entry.getName();
                String lower = name.toLowerCase(Locale.ROOT);
                if (entry.isDirectory()) continue;
                if (lower.endsWith(".tar.xz")) {
                    File tar = new File(root, "archive-" + (count++));
                    try (OutputStream target = new BufferedOutputStream(new FileOutputStream(tar))) {
                        int read;
                        while ((read = input.read(buffer)) != -1) target.write(buffer, 0, read);
                    }
                    extractTarXz(tar, root, output);
                    continue;
                }
                if (!lower.endsWith(".apk")) continue;
                File apk = new File(root, "apk-" + (count++));
                try (OutputStream target = new BufferedOutputStream(new FileOutputStream(apk))) {
                    int read;
                    while ((read = input.read(buffer)) != -1) target.write(buffer, 0, read);
                }
                output.add(apk);
            }
        }
    }

    private static boolean isZip(File file) throws Exception {
        try (ZipFile ignored = new ZipFile(file)) { return true; }
        catch (Exception error) { return false; }
    }
    private static boolean isXz(File file) throws Exception {
        try (InputStream input = new FileInputStream(file)) {
            return input.read() == 0xfd && input.read() == 0x37 && input.read() == 0x7a && input.read() == 0x58;
        }
    }
    private static void copy(InputStream input, OutputStream output) throws Exception {
        byte[] buffer = new byte[32768];
        int read;
        while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
    }

    private static void extractTarXz(File archive, File root, List<File> output) throws Exception {
        try (InputStream xz = new BufferedInputStream(new XZInputStream(new FileInputStream(archive)))) {
            byte[] header = new byte[512];
            int sequence = 0;
            while (readBlock(xz, header)) {
                boolean empty = true;
                for (byte value : header) if (value != 0) { empty = false; break; }
                if (empty) break;
                String name = tarString(header, 0, 100);
                String prefix = tarString(header, 345, 155);
                if (!prefix.isEmpty()) name = prefix + "/" + name;
                long size = tarOctal(header, 124, 12);
                byte kind = header[156];
                String lower = name.toLowerCase(Locale.ROOT);
                if ((kind == 0 || kind == '0') && lower.endsWith(".apk")) {
                    File apk = new File(root, "tar-apk-" + (sequence++));
                    try (OutputStream target = new BufferedOutputStream(new FileOutputStream(apk))) {
                        copyExactly(xz, target, size);
                    }
                    output.add(apk);
                } else {
                    skipExactly(xz, size);
                }
                long padding = (512 - (size % 512)) % 512;
                skipExactly(xz, padding);
            }
        }
    }
    private static boolean readBlock(InputStream input, byte[] block) throws Exception {
        int offset = 0;
        while (offset < block.length) {
            int count = input.read(block, offset, block.length - offset);
            if (count < 0) return offset == 0 ? false : throwTruncatedTar();
            offset += count;
        }
        return true;
    }
    private static boolean throwTruncatedTar() throws Exception { throw new IllegalArgumentException("LiteGapps archive has a truncated tar header."); }
    private static String tarString(byte[] data, int offset, int length) {
        int end = offset;
        while (end < offset + length && data[end] != 0) end++;
        return new String(data, offset, end - offset, java.nio.charset.StandardCharsets.UTF_8);
    }
    private static long tarOctal(byte[] data, int offset, int length) {
        String value = tarString(data, offset, length).trim();
        return value.isEmpty() ? 0 : Long.parseLong(value, 8);
    }
    private static void copyExactly(InputStream input, OutputStream output, long length) throws Exception {
        byte[] buffer = new byte[32768];
        long remaining = length;
        while (remaining > 0) {
            int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
            if (count < 0) throw new IllegalArgumentException("LiteGapps archive has a truncated APK.");
            output.write(buffer, 0, count);
            remaining -= count;
        }
    }
    private static void skipExactly(InputStream input, long length) throws Exception {
        long remaining = length;
        while (remaining > 0) {
            long count = input.skip(remaining);
            if (count <= 0) {
                if (input.read() < 0) throw new IllegalArgumentException("LiteGapps archive is truncated.");
                count = 1;
            }
            remaining -= count;
        }
    }

    private static List<Apk> validate(Context context, List<File> files) throws Exception {
        PackageManager pm = context.getPackageManager();
        Map<String, Apk> found = new HashMap<>();
        for (File file : files) {
            PackageInfo info = pm.getPackageArchiveInfo(file.getAbsolutePath(), PackageManager.GET_SIGNING_CERTIFICATES);
            if (info == null || info.packageName == null || !isSupportedPackage(info.packageName)) continue;
            if (!isX86_64OrNoNativeCode(file)) throw new IllegalArgumentException("ABI check failed for " + info.packageName + ": native code must include x86_64.");
            final long baseline;
            try {
                PackageInfo installed = pm.getPackageInfo(info.packageName, 0);
                baseline = installed.getLongVersionCode();
            } catch (PackageManager.NameNotFoundException absent) {
                throw new IllegalArgumentException("Version check failed for " + info.packageName + ": no preinstalled stand-in exists to update.");
            }
            if (info.getLongVersionCode() <= baseline) throw new IllegalArgumentException("Version check failed for " + info.packageName + ": the APK is not newer than the preinstalled stand-in.");
            if (!hasGoogleSigningLineage(info)) throw new IllegalArgumentException("Google signing lineage check failed for " + info.packageName + ": the certificate history does not reach Google's pinned root.");
            if (isSupportedPackage(info.packageName)) found.put(info.packageName, new Apk(info.packageName, file));
        }
        List<Apk> result = new ArrayList<>(found.values());
        if (result.isEmpty()) throw new IllegalArgumentException("No supported Google Play services, Play Store, or Services Framework APK passed validation.");
        Collections.sort(result, Comparator.comparingInt(apk -> installOrder(apk.packageName)));
        return result;
    }

    private static boolean isSupportedPackage(String name) {
        return "com.google.android.gms".equals(name) || "com.android.vending".equals(name) || "com.google.android.gsf".equals(name);
    }
    private static int installOrder(String name) {
        if ("com.google.android.gsf".equals(name)) return 0;
        if ("com.google.android.gms".equals(name)) return 1;
        return 2;
    }
    private static boolean isX86_64OrNoNativeCode(File apk) throws Exception {
        try (ZipFile zip = new ZipFile(apk)) {
            boolean nativeCode = false;
            boolean x86_64 = false;
            java.util.Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                String name = entries.nextElement().getName();
                if (!name.startsWith("lib/")) continue;
                nativeCode = true;
                if (name.startsWith("lib/x86_64/")) x86_64 = true;
            }
            return !nativeCode || x86_64;
        }
    }
    private static boolean hasGoogleSigningLineage(PackageInfo info) throws Exception {
        SigningInfo signing = info.signingInfo;
        if (signing == null) return false;
        Signature[] history = signing.getSigningCertificateHistory();
        if (history == null) return false;
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        for (Signature signature : history) {
            StringBuilder hex = new StringBuilder(64);
            for (byte value : digest.digest(signature.toByteArray())) hex.append(String.format(Locale.ROOT, "%02X", value & 0xff));
            if (GOOGLE_ROOT.equals(hex.toString())) return true;
        }
        return false;
    }

    private static void installNext(Activity activity, List<Apk> apks, int index, Promise promise, List<String> installed) throws Exception {
        if (index >= apks.size()) {
            GappsPickerActivity.pendingInstall = null;
            promise.resolve(map("cancelled", false, "ok", true, "installed", installed, "message", "Validated updates were installed."));
            activity.runOnUiThread(activity::finish);
            return;
        }
        Apk apk = apks.get(index);
        PackageInstaller installer = activity.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(apk.packageName);
        int sessionId = installer.createSession(params);
        GappsPickerActivity.pendingInstall = new PendingInstall(activity, apks, index, promise, installed);
        try (PackageInstaller.Session session = installer.openSession(sessionId); InputStream input = new FileInputStream(apk.file); OutputStream output = session.openWrite(apk.packageName, 0, apk.file.length())) {
            copy(input, output);
            session.fsync(output);
            Intent callback = new Intent(activity, GappsInstallResultReceiver.class).setAction("org.matonos.settings.GAPPS_INSTALL_RESULT").putExtra("sessionId", sessionId);
            PendingIntent pending = PendingIntent.getBroadcast(activity, sessionId, callback, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pending.getIntentSender());
        } catch (Exception error) {
            throw error;
        }
    }

    static void onInstallResult(Context context, Intent intent) {
        int sessionId = intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, intent.getIntExtra("sessionId", -1));
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        PendingInstall pending = GappsPickerActivity.pendingInstall;
        if (pending == null) return;
        if (status == PackageInstaller.STATUS_SUCCESS) {
            pending.installed.add(pending.apks.get(pending.index).packageName);
            WORKER.execute(() -> {
                try {
                    installNext(pending.activity, pending.apks, pending.index + 1, pending.promise, pending.installed);
                } catch (Exception error) {
                    GappsPickerActivity.pendingInstall = null;
                    pending.promise.resolve(map("cancelled", false, "ok", false, "message", "Install failed after " + pending.installed + ": " + safeMessage(error)));
                    pending.activity.runOnUiThread(pending.activity::finish);
                }
            });
        } else if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) pending.activity.startActivity(confirm);
        } else {
            String detail = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            GappsPickerActivity.pendingInstall = null;
            pending.promise.resolve(map("cancelled", false, "ok", false, "message", "Install failed for " + pending.apks.get(pending.index).packageName + (detail == null ? "." : ": " + detail)));
            pending.activity.finish();
        }
    }

    /**
     * GSF Android ID (decimal), the ID Google's uncertified-device registration asks for.
     * It exists only once a user-supplied GSF is installed and has checked in; empty before.
     */
    public static String deviceId(Context context) {
        Uri gservices = Uri.parse("content://com.google.android.gsf.gservices");
        try (android.database.Cursor cursor = context.getContentResolver().query(
                gservices, null, null, new String[] {"android_id"}, null)) {
            if (cursor == null || !cursor.moveToFirst() || cursor.getColumnCount() < 2) return "";
            String value = cursor.getString(1);
            return value == null ? "" : value;
        } catch (SecurityException | IllegalArgumentException e) {
            android.util.Log.i("MatonGapps", "GSF ID unavailable: " + e.getMessage());
            return "";
        }
    }
    public static void uninstallUpdates(Context context, Promise promise) {
        try {
            ArrayList<String> packages = new ArrayList<>();
            for (String name : Arrays.asList("com.google.android.gsf", "com.google.android.gms", "com.android.vending")) {
                try {
                    PackageInfo info = context.getPackageManager().getPackageInfo(name, 0);
                    if ((info.applicationInfo.flags & android.content.pm.ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) != 0) packages.add(name);
                } catch (PackageManager.NameNotFoundException ignored) { }
            }
            if (packages.isEmpty()) {
                promise.resolve(map("ok", true, "message", "No GApps updates are installed."));
                return;
            }
            GappsInstallResultReceiver.uninstallPromise = promise;
            GappsInstallResultReceiver.uninstallRemaining = packages;
            uninstallNext(context);
        } catch (Exception error) {
            promise.resolve(map("ok", false, "message", safeMessage(error)));
        }
    }
    private static void uninstallNext(Context context) throws Exception {
        List<String> remaining = GappsInstallResultReceiver.uninstallRemaining;
        if (remaining == null || remaining.isEmpty()) {
            Promise promise = GappsInstallResultReceiver.uninstallPromise;
            GappsInstallResultReceiver.uninstallPromise = null;
            GappsInstallResultReceiver.uninstallRemaining = null;
            if (promise != null) promise.resolve(map("ok", true, "message", "GApps updates removed. The preinstalled apps are restored."));
            return;
        }
        String packageName = remaining.remove(0);
        Intent callback = new Intent(context, GappsInstallResultReceiver.class).setAction("org.matonos.settings.GAPPS_UNINSTALL_RESULT");
        PendingIntent pending = PendingIntent.getBroadcast(context, 9981, callback, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
        context.getPackageManager().getPackageInstaller().uninstall(packageName, pending.getIntentSender());
    }
    static void onUninstallResult(Context context, Intent intent) {
        Promise promise = GappsInstallResultReceiver.uninstallPromise;
        if (promise == null) return;
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status != PackageInstaller.STATUS_SUCCESS) {
            promise.resolve(map("ok", false, "message", "Could not remove GApps update: " + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)));
            GappsInstallResultReceiver.uninstallPromise = null;
            GappsInstallResultReceiver.uninstallRemaining = null;
            return;
        }
        try { uninstallNext(context); }
        catch (Exception error) {
            promise.resolve(map("ok", false, "message", "Could not remove the next GApps update: " + safeMessage(error)));
            GappsInstallResultReceiver.uninstallPromise = null;
            GappsInstallResultReceiver.uninstallRemaining = null;
        }
    }

    private static String safeMessage(Exception error) { return error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage(); }
    private static Map<String, Object> map(Object... values) {
        Map<String, Object> result = new HashMap<>();
        for (int index = 0; index + 1 < values.length; index += 2) result.put((String) values[index], values[index + 1]);
        return result;
    }
    static final class Apk {
        final String packageName; final File file;
        Apk(String name, File source) { packageName = name; file = source; }
    }
    static final class PendingInstall {
        final Activity activity; final List<Apk> apks; final int index; final Promise promise; final List<String> installed;
        PendingInstall(Activity a, List<Apk> files, int i, Promise p, List<String> done) { activity = a; apks = files; index = i; promise = p; installed = done; }
    }
}
