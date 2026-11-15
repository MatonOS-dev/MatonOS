package org.matonos.flathub.sources;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import org.matonos.store.source.IInstallCallback;
import org.matonos.store.source.ISoftwareSource;

import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Finds, enables and talks to store source plugins. Preinstalled plugins are
 * enabled by default; any other plugin stays off until the user enables it.
 * Only the store commits APKs: plugins hand them over through IInstallCallback.
 */
public final class SourcePlugins {
  public static final String ACTION = "org.matonos.store.SOURCE";
  /** Signature permission defined by the store; a plugin service must require it. */
  public static final String BIND_PERMISSION = "org.matonos.permission.BIND_STORE_SOURCE";
  private static final String TAG = "SourcePlugins";
  private static final String PREFS = "source-plugins";
  private static final long BIND_TIMEOUT_S = 20;

  public interface Events {
    void progress(String source, String operationId, String phase, int percent, long done, long total);
    void result(String source, String operationId, boolean ok, String error);
    /** Commit result intent for one PackageInstaller session. */
    android.content.IntentSender commitTarget(String source, String operationId);
  }

  private final Context context;
  private final Events events;
  private final Map<String, Connection> connections = new ConcurrentHashMap<>();

  public SourcePlugins(Context context, Events events) {
    this.context = context.getApplicationContext();
    this.events = events;
  }

  /** Every installed plugin service that requires the store's bind permission. */
  public List<Map<String, Object>> list() {
    PackageManager pm = context.getPackageManager();
    SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    List<Map<String, Object>> out = new ArrayList<>();
    for (ResolveInfo info : pm.queryIntentServices(new Intent(ACTION), 0)) {
      if (!BIND_PERMISSION.equals(info.serviceInfo.permission)) {
        Log.w(TAG, info.serviceInfo.packageName + " offers a source without requiring " + BIND_PERMISSION + "; ignored");
        continue;
      }
      ApplicationInfo app = info.serviceInfo.applicationInfo;
      boolean preinstalled = (app.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
      Map<String, Object> row = new HashMap<>();
      row.put("package", app.packageName);
      row.put("service", info.serviceInfo.name);
      row.put("label", String.valueOf(info.loadLabel(pm)));
      row.put("preinstalled", preinstalled);
      row.put("enabled", prefs.getBoolean(app.packageName, preinstalled));
      out.add(row);
    }
    return out;
  }

  public void setEnabled(String pkg, boolean enabled) {
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(pkg, enabled).apply();
    if (!enabled) disconnect(pkg);
  }

  /** Binds (blocking, off the main thread) to an enabled plugin. */
  public ISoftwareSource source(String pkg) throws Exception {
    Map<String, Object> row = null;
    for (Map<String, Object> candidate : list()) if (pkg.equals(candidate.get("package"))) row = candidate;
    if (row == null) throw new IllegalArgumentException("No store source plugin in " + pkg);
    if (!(Boolean) row.get("enabled")) throw new IllegalStateException("Store source " + pkg + " is disabled");
    Connection connection = connections.get(pkg);
    if (connection == null || connection.binder == null || !connection.binder.isBinderAlive()) {
      if (connection != null) disconnect(pkg);
      connection = new Connection();
      Intent intent = new Intent(ACTION).setClassName(pkg, (String) row.get("service"));
      if (!context.bindService(intent, connection, Context.BIND_AUTO_CREATE))
        throw new IllegalStateException("Cannot bind store source " + pkg);
      connections.put(pkg, connection);
      if (!connection.ready.await(BIND_TIMEOUT_S, TimeUnit.SECONDS)) {
        disconnect(pkg);
        throw new IllegalStateException("Store source " + pkg + " did not connect within " + BIND_TIMEOUT_S + " s");
      }
    }
    return ISoftwareSource.Stub.asInterface(connection.binder);
  }

  public void install(String pkg, String id, String operationId) throws Exception {
    source(pkg).install(id, operationId, new IInstallCallback.Stub() {
      @Override public void progress(String op, String phase, int percent, long done, long total) {
        events.progress(pkg, op, phase, percent, done, total);
      }
      @Override public void apks(String op, String packageName, ParcelFileDescriptor[] files) {
        try {
          commit(pkg, op, packageName, files);
        } catch (Exception error) {
          Log.e(TAG, pkg + ": committing " + packageName + " failed", error);
          events.result(pkg, op, false, String.valueOf(error.getMessage()));
        } finally {
          for (ParcelFileDescriptor file : files) try { file.close(); } catch (Exception ignored) { }
        }
      }
      @Override public void failed(String op, String error) {
        Log.e(TAG, pkg + ": install " + id + " failed: " + error);
        events.result(pkg, op, false, error);
      }
    });
  }

  private void commit(String source, String operationId, String packageName, ParcelFileDescriptor[] files) throws Exception {
    if (files == null || files.length == 0) throw new IllegalArgumentException(source + " sent no APKs for " + packageName);
    PackageInstaller installer = context.getPackageManager().getPackageInstaller();
    PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
    params.setAppPackageName(packageName);
    params.setRequestUpdateOwnership(true);
    int sessionId = installer.createSession(params);
    try (PackageInstaller.Session session = installer.openSession(sessionId)) {
      for (int i = 0; i < files.length; i++) {
        long size = files[i].getStatSize();
        try (InputStream in = new FileInputStream(files[i].getFileDescriptor());
             OutputStream out = session.openWrite(i == 0 ? "base.apk" : "split_" + i + ".apk", 0, size)) {
          byte[] buffer = new byte[64 * 1024];
          int count;
          while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
          session.fsync(out);
        }
      }
      // PackageInstaller rejects a session whose APKs name another package
      // than setAppPackageName, so a plugin cannot slip in a different app.
      events.progress(source, operationId, "Installing", -1, -1, -1);
      session.commit(events.commitTarget(source, operationId));
    } catch (Exception error) {
      try { installer.abandonSession(sessionId); } catch (Exception ignored) { }
      throw error;
    }
  }

  public void uninstall(String source, String packageName, String operationId) {
    context.getPackageManager().getPackageInstaller().uninstall(packageName, events.commitTarget(source, operationId));
  }

  public void close() {
    for (String pkg : new ArrayList<>(connections.keySet())) disconnect(pkg);
  }

  private void disconnect(String pkg) {
    Connection connection = connections.remove(pkg);
    if (connection != null) try { context.unbindService(connection); } catch (Exception ignored) { }
  }

  private static final class Connection implements ServiceConnection {
    final CountDownLatch ready = new CountDownLatch(1);
    volatile IBinder binder;
    @Override public void onServiceConnected(ComponentName name, IBinder service) { binder = service; ready.countDown(); }
    @Override public void onServiceDisconnected(ComponentName name) { binder = null; }
    @Override public void onBindingDied(ComponentName name) { binder = null; }
  }
}
