package org.matonos.compositor.stub;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.util.Log;
import org.json.JSONArray;
import org.json.JSONObject;
import org.matonos.compositor.IEmbeddedSession;
import org.matonos.compositor.PerAppRuntime;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.Map;

/** One Linux process tree per stub UID, under the inherited app filter/domain. */
final class V3Launcher {
    private static Process process;
    private static Integer exit;
    private static String error;

    static synchronized JSONObject launch(Context context, IEmbeddedSession session) throws Exception {
        if (process != null) return getLaunchStatus();
        JSONObject response = new JSONObject(session.launch());
        if (!response.optBoolean("ok")) return response;
        JSONObject spec = response;
        // PackageManager follows signature-verified updates of the image host.
        ApplicationInfo host = context.getPackageManager().getApplicationInfo("org.matonos.compositor", 0);
        File loader = new File(host.nativeLibraryDir, "libmatonos-v3-loader.so");
        if (!loader.isFile() || !loader.canExecute())
            throw new IllegalStateException("Update the Linux host: v3 loader is missing");
        PerAppRuntime runtime = StubService.runtime();
        if (runtime == null) throw new IllegalStateException("Application runtime is unavailable");
        File home = new File(context.getDataDir(), "home");
        File cache = context.getCacheDir();
        for (File directory : new File[]{home, cache, new File(home, ".config"), new File(home, ".local/share")})
            if (!directory.isDirectory() && !directory.mkdirs())
                throw new java.io.IOException("Cannot create application directory " + directory);
        File resolv = new File(cache, "v3-resolv.conf");
        JSONObject supplied = spec.getJSONObject("env");
        String dns = supplied.getString("MATON_FLATPAK_DNS");
        if (!dns.matches("127\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}"))
            throw new IllegalArgumentException("Invalid per-app DNS endpoint");
        Files.write(resolv.toPath(), ("nameserver " + dns + "\n").getBytes(StandardCharsets.UTF_8));
        ArrayList<String> command = new ArrayList<>();
        command.add(loader.getAbsolutePath());
        if (!DnsForwarder.endpoint().equals(dns + ":53") || DnsForwarder.acquire() == null)
            throw new IllegalStateException("Per-app DNS forwarder is unavailable");
        command.add("--dns-forwarder"); command.add(dns + ":" + DnsForwarder.port);
        command.add("--parent-pid"); command.add(Integer.toString(android.os.Process.myPid()));
        command.add("--map"); command.add(resolv.getAbsolutePath()); command.add("/etc/resolv.conf");
        File x11 = new File(runtime.directory(), "x11");
        if (x11.isDirectory()) {
            command.add("--map"); command.add(x11.getAbsolutePath()); command.add("/tmp/.X11-unix");
        }
        JSONArray argv = spec.getJSONArray("argv");
        for (int i = 0; i < argv.length(); ++i) command.add(argv.getString(i));
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        Map<String, String> env = builder.environment();
        env.clear();
        for (Iterator<String> keys = supplied.keys(); keys.hasNext();) {
            String key = keys.next(); env.put(key, supplied.getString(key));
        }
        env.put("PATH", "/apex/com.matonos.flatpak/bin:/system/bin");
        env.put("LD_LIBRARY_PATH", "/apex/com.matonos.flatpak/lib64:/system/lib64");
        env.put("HOME", home.getAbsolutePath());
        env.put("TMPDIR", cache.getAbsolutePath());
        env.put("FLATPAK_DOWNLOAD_TMPDIR", cache.getAbsolutePath());
        env.put("FLATPAK_SYSTEM_CACHE_DIR", cache.getAbsolutePath());
        env.put("XDG_CACHE_HOME", cache.getAbsolutePath());
        env.put("XDG_CONFIG_HOME", new File(home, ".config").getAbsolutePath());
        env.put("XDG_DATA_HOME", new File(home, ".local/share").getAbsolutePath());
        env.put("XDG_RUNTIME_DIR", runtime.directory().getAbsolutePath());
        env.put("WAYLAND_DISPLAY", "wayland-0");
        env.put("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + new File(runtime.directory(), "bus"));
        env.put("PULSE_SERVER", "unix:" + new File(runtime.directory(), "pulse/native"));
        File alias = new File(runtime.directory(), "wayland-0-x11");
        if (Files.isSymbolicLink(alias.toPath())) {
            String target = Files.readSymbolicLink(alias.toPath()).toString();
            if (target.matches("x11/X[0-9]+")) env.put("DISPLAY", ":" + target.substring(5));
        }
        builder.directory(home);
        process = builder.start();
        final Process child = process;
        Log.i("MatonV3", "Started v3 loader for " + context.getPackageName());
        new Thread(() -> {
            try (BufferedReader output = new BufferedReader(new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = output.readLine()) != null) Log.i("MatonV3", line);
            } catch (Exception failure) { Log.w("MatonV3", "Cannot read loader output", failure); }
        }, "v3-output").start();
        new Thread(() -> {
            try {
                int status = child.waitFor();
                synchronized (V3Launcher.class) { exit = status; }
                Log.i("MatonV3", "Loader exited: " + status);
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                synchronized (V3Launcher.class) { error = "Interrupted while waiting for loader"; }
            }
        }, "v3-exit").start();
        return getLaunchStatus();
    }

    static synchronized JSONObject getLaunchStatus() throws Exception {
        JSONObject status = new JSONObject().put("ok", process != null && exit == null && error == null);
        if (exit != null) status.put("exitCode", exit).put("error", "Application exited (" + exit + ")");
        if (error != null) status.put("error", error);
        return status;
    }
}
