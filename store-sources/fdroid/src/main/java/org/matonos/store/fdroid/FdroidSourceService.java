package org.matonos.store.fdroid;

import android.app.Service;
import android.content.Intent;
import android.content.pm.*;
import android.os.*;
import android.util.Log;
import org.json.JSONArray;
import org.json.JSONObject;
import org.matonos.store.source.*;
import java.io.*;
import java.net.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Signed F-Droid catalogues and verified downloads; only the store installs packages. */
public final class FdroidSourceService extends Service {
  private static final String TAG = "FdroidSource";
  private FdroidRepositories repositories;
  private FdroidIndexClient client;
  @Override public void onCreate() {
    super.onCreate(); repositories = new FdroidRepositories(this);
    Set<String> features = new HashSet<>();
    for (FeatureInfo f : getPackageManager().getSystemAvailableFeatures()) if (f.name != null) features.add(f.name);
    client = new FdroidIndexClient(37, Arrays.asList(Build.SUPPORTED_ABIS), features);
  }
  private interface Call { String run() throws Exception; }
  private String call(String context, Call call) {
    try { return call.run(); }
    catch (Exception error) { Log.e(TAG, context + " failed", error); throw new IllegalStateException(context + ": " + error.getMessage(), error); }
  }
  private final ISoftwareSource.Stub binder = new ISoftwareSource.Stub() {
    @Override public String info() { return "{\"version\":1,\"name\":\"F-Droid\",\"label\":\"F-Droid\",\"capabilities\":[\"HAS_REPOSITORIES\"]}"; }
    @Override public String search(String term) { return call("search " + term, () -> new JSONArray(listings(catalogue("search", term))).toString()); }
    @Override public String details(String id) { return call("details " + id, () -> new JSONObject(find(id)).toString()); }
    @Override public String reviews(String id) { return "[]"; }
    @Override public String installed() { return call("installed", () -> new JSONArray(listings(catalogue("installed", ""))).toString()); }
    @Override public String updates() { return call("updates", () -> new JSONArray(listings(catalogue("updates", ""))).toString()); }
    @Override public String repos() { return call("repos", () -> new JSONArray(repositories.list()).toString()); }
    @Override public void addRepo(String url) { call("addRepo " + url, () -> {
      String repo = repositories.add(url);
      for (Map<String, Object> r : repositories.list()) if (repo.equals(r.get("url"))) {
        try { client.search(getFilesDir(), repo, text(r.get("fingerprint")), ""); repositories.setBroken(repo, false); }
        catch (Exception error) { Log.e(TAG, "Verifying added repository " + repo + " failed", error); throw error; }
      }
      return "";
    }); }
    @Override public void delRepo(String id) { call("delRepo " + id, () -> { repositories.remove(id); return ""; }); }
    @Override public void install(String id, String op, IInstallCallback callback) {
      if (callback == null) throw new IllegalArgumentException("Install callback required");
      new Thread(() -> prepare(id, op, callback), "fdroid-install").start();
    }
  };
  @Override public IBinder onBind(Intent intent) { return binder; }

  private List<Map<String, Object>> listings(List<Map<String, Object>> apps) {
    List<Map<String, Object>> result = new ArrayList<>();
    for (Map<String, Object> app : apps) {
      Map<String, Object> row = new LinkedHashMap<>();
      for (String key : new String[]{"id", "packageName", "name", "summary", "icon", "developer", "installed", "versionName", "origin"}) if (app.containsKey(key)) row.put(key, app.get(key));
      result.add(row);
    }
    return result;
  }

  private List<Map<String, Object>> installedVersions() throws Exception {
    List<Map<String, Object>> result = new ArrayList<>();
    for (PackageInfo p : getPackageManager().getInstalledPackages(PackageManager.PackageInfoFlags.of(PackageManager.GET_SIGNING_CERTIFICATES))) {
      if (p.signingInfo == null) continue;
      List<String> signers = new ArrayList<>();
      for (Signature s : p.signingInfo.getApkContentsSigners()) {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(s.toByteArray())) hex.append(String.format(Locale.ROOT, "%02x", b & 255));
        signers.add(hex.toString());
      }
      Collections.sort(signers);
      Map<String, Object> row = new LinkedHashMap<>(); row.put("packageName", p.packageName); row.put("versionCode", p.getLongVersionCode()); row.put("signerSha256", signers); result.add(row);
    }
    return result;
  }
  private List<Map<String, Object>> catalogue(String mode, String term) throws Exception {
    Map<String, Map<String, Object>> unique = new LinkedHashMap<>();
    List<Map<String, Object>> installed = mode.equals("search") ? Collections.emptyList() : installedVersions();
    List<String> names = new ArrayList<>(); for (Map<String, Object> p : installed) names.add(text(p.get("packageName")));
    int succeeded = 0; Exception last = null;
    for (Map<String, Object> repo : repositories.list()) {
      String url = text(repo.get("url")), fingerprint = text(repo.get("fingerprint"));
      try {
        List<Map<String, Object>> apps;
        if (mode.equals("updates")) apps = client.updatesCatalogue(getFilesDir(), url, fingerprint, installed);
        else if (mode.equals("installed")) apps = client.installedCatalogue(getFilesDir(), url, fingerprint, names);
        else apps = client.search(getFilesDir(), url, fingerprint, term);
        repositories.setBroken(url, false); succeeded++;
        for (Map<String, Object> app : apps) {
          if (mode.equals("installed")) {
            Map<String, Object> current = null;
            for (Map<String, Object> p : installed) if (p.get("packageName").equals(app.get("id"))) current = p;
            if (current == null || !current.get("signerSha256").equals(app.get("signerSha256"))) continue;
          }
          decorate(app, url); if (!mode.equals("search")) app.put("installed", true);
          Map<String, Object> previous = unique.get(text(app.get("id")));
          if (previous == null || number(app.get("versionCode")) > number(previous.get("versionCode"))) unique.put(text(app.get("id")), app);
        }
      } catch (Exception error) { Log.e(TAG, mode + " repository=" + url + " failed", error); repositories.setBroken(url, true); last = error; }
    }
    if (succeeded == 0 && last != null) throw last;
    return new ArrayList<>(unique.values());
  }
  private Map<String, Object> find(String id) throws Exception {
    Map<String, Object> newest = null;
    Map<String, Object> existing = null;
    for (Map<String, Object> installed : installedVersions()) if (id.equals(installed.get("packageName"))) existing = installed;
    for (Map<String, Object> repo : repositories.list()) {
      String url = text(repo.get("url"));
      try {
        Map<String, Object> app = client.details(getFilesDir(), url, text(repo.get("fingerprint")), id);
        repositories.setBroken(url, false);
        if (app != null && existing != null && !existing.get("signerSha256").equals(app.get("signerSha256"))) continue;
        if (app != null && (newest == null || number(app.get("versionCode")) > number(newest.get("versionCode")))) { decorate(app, url); newest = app; }
      } catch (Exception error) { Log.e(TAG, "details id=" + id + " repository=" + url + " failed", error); repositories.setBroken(url, true); }
    }
    if (newest == null) throw new IllegalArgumentException("No compatible F-Droid version for " + id);
    return newest;
  }
  private void decorate(Map<String, Object> app, String url) {
    app.put("origin", url); app.put("repository", url);
    String icon = text(app.get("icon"));
    if (icon.startsWith(FdroidRepositories.OFFICIAL_URL + "/")) app.put("icon", url + icon.substring(FdroidRepositories.OFFICIAL_URL.length()));
    FdroidSafety.apply(app);
  }
  private void prepare(String id, String op, IInstallCallback callback) {
    File cache = null; List<ParcelFileDescriptor> descriptors = new ArrayList<>();
    AtomicBoolean terminal = new AtomicBoolean();
    try {
      callback.progress(op, "Preparing", -1, 0, -1);
      Map<String, Object> app = find(id);
      cache = java.nio.file.Files.createTempDirectory(getCacheDir().toPath(), "fdroid-").toFile();
      List<?> files = (List<?>) app.get("apkFiles"); List<String> names = new ArrayList<>();
      int index = 0;
      for (Object raw : files) {
        Map<?, ?> file = (Map<?, ?>) raw; File target = new File(cache, index + ".apk");
        download(text(app.get("origin")), text(file.get("name")), target, op, callback, index++, files.size());
        new FdroidApkInstaller().verify(target, text(file.get("sha256")));
        String name = ApkManifest.sessionName(target, id);
        if (names.contains(name)) throw new IllegalArgumentException("Duplicate APK split " + name);
        names.add(name); descriptors.add(ParcelFileDescriptor.open(target, ParcelFileDescriptor.MODE_READ_ONLY));
      }
      if (!names.contains("base.apk")) throw new IllegalArgumentException("F-Droid package has no base APK");
      callback.progress(op, "Verified APKs", 100, 0, 0);
      callback.commit(op, id, false, names.toArray(new String[0]), descriptors.toArray(new ParcelFileDescriptor[0]), new String[0], new ICommitResult.Stub() {
        @Override public void result(boolean ok, String error) {
          if (!terminal.compareAndSet(false, true)) { Log.e(TAG, "Duplicate commit result op=" + op); return; }
          try { if (ok) callback.done(op); else { Log.e(TAG, "Commit failed op=" + op + " package=" + id + ": " + error); callback.failed(op, error); } }
          catch (RemoteException gone) { Log.e(TAG, "Delivering terminal result op=" + op + " failed", gone); }
        }
      });
    } catch (Exception error) {
      Log.e(TAG, "Install op=" + op + " package=" + id + " failed", error);
      if (terminal.compareAndSet(false, true)) try { callback.failed(op, text(error.getMessage())); } catch (RemoteException gone) { Log.e(TAG, "Reporting failed install op=" + op, gone); }
    } finally {
      for (ParcelFileDescriptor fd : descriptors) { Log.i(TAG, "close fd=" + fd.getFd() + " at FdroidSourceService.prepare"); try { fd.close(); } catch (IOException error) { Log.e(TAG, "Closing APK descriptor op=" + op, error); } }
      if (cache != null) { File[] files = cache.listFiles(); if (files == null) Log.e(TAG, "Cannot list install cache " + cache); else for (File file : files) if (!file.delete()) Log.e(TAG, "Cannot delete " + file); if (!cache.delete()) Log.e(TAG, "Cannot delete " + cache); }
    }
  }
  private void download(String repo, String path, File destination, String op, IInstallCallback callback, int index, int count) throws Exception {
    String name = path.replaceFirst("^/+", "");
    if (Arrays.asList(name.split("/")).contains("..")) throw new IllegalArgumentException("Invalid APK path " + path);
    HttpURLConnection connection = (HttpURLConnection) new URL(repo + "/" + name).openConnection();
    connection.setConnectTimeout(20000); connection.setReadTimeout(60000);
    try {
      int status = connection.getResponseCode(); if (status < 200 || status >= 300) throw new IOException("HTTP " + status + " downloading " + connection.getURL());
      long total = connection.getContentLengthLong(), done = 0, reported = 0;
      try (InputStream in = connection.getInputStream(); FileOutputStream out = new FileOutputStream(destination)) {
        byte[] buffer = new byte[65536]; int n;
        while ((n = in.read(buffer)) != -1) {
          out.write(buffer, 0, n); done += n;
          long now = SystemClock.elapsedRealtime();
          if (now - reported >= 250) { callback.progress(op, "Downloading APK " + (index + 1) + " of " + count, total > 0 ? (int)((index * 100L + done * 100 / total) / count) : -1, done, total); reported = now; }
        }
      }
    } finally { connection.disconnect(); }
  }
  private static String text(Object value) { return value == null ? "" : String.valueOf(value); }
  private static long number(Object value) { return value instanceof Number ? ((Number)value).longValue() : 0; }
}
