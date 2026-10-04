package org.matonos.compositor;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/** One native broker, owned and reaped by the compositor Wayland session. */
final class SessionBus implements AutoCloseable {
    private final Process process;
    private final File socket, control, policy;

    SessionBus(android.content.Context context, File directory, String ref) throws Exception {
        android.content.pm.ApplicationInfo appInfo = context.getApplicationInfo();
        // PM never extracts the bundled system APK. The image build extracts
        // its broker from that APK into an executable system_file path. Updates
        // must use their own extracted broker; never mix in the image version.
        boolean bundled = (appInfo.flags & android.content.pm.ApplicationInfo.FLAG_SYSTEM) != 0
                && (appInfo.flags & android.content.pm.ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) == 0;
        File executable = bundled ? new File("/system_ext/bin/matonos-apk-session-broker")
                : new File(appInfo.nativeLibraryDir, "libmatonos-dbus-broker.so");
        if (!executable.isFile() || !executable.canExecute())
            throw new java.io.IOException("APK session broker is missing or not executable: " + executable);
        if (!ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid application reference");
        String app = ref.split("/")[1];
        socket = new File(directory, "bus");
        control = new File(directory, "bus-control");
        policy = new File(directory, "bus.policy");
        // A stale broker must have died with its host before these are removed.
        for (File file : new File[]{socket, control, policy})
            if (file.exists() && !file.delete()) throw new java.io.IOException("Cannot remove stale bus file");
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(policy)) {
            out.write(("own " + app + "\ntalk org.freedesktop.portal.Flatpak\ntalk org.freedesktop.portal.Desktop\ntalk org.freedesktop.Flatpak\n").getBytes(StandardCharsets.UTF_8));
        }
        android.system.Os.chmod(policy.getAbsolutePath(), 0600);
        ProcessBuilder builder = new ProcessBuilder(executable.getAbsolutePath(),
                socket.getAbsolutePath(), policy.getAbsolutePath(), "--host-session");
        builder.redirectError(ProcessBuilder.Redirect.appendTo(new File(directory, "bus.log")));
        try { process = builder.start(); }
        catch (Exception error) { policy.delete(); throw error; }
        FutureTask<String> ready = new FutureTask<>(() -> {
            try (java.io.BufferedReader reader = new java.io.BufferedReader(
                    new java.io.InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                return reader.readLine();
            }
        });
        new Thread(ready, "session-bus-ready").start();
        try {
            String line = ready.get(5, TimeUnit.SECONDS);
            if (!process.isAlive() || !("DBUS_SESSION_BUS_ADDRESS=unix:path=" + socket.getAbsolutePath()).equals(line))
                throw new java.io.IOException("Session broker failed to become ready");
        } catch (Exception error) { close(); throw error; }
        finally { policy.delete(); }
    }
    boolean isAlive() { return process.isAlive(); }
    @Override public void close() {
        process.destroy();
        try {
            if (!process.waitFor(2, TimeUnit.SECONDS)) { process.destroyForcibly(); process.waitFor(); }
        } catch (InterruptedException error) { Thread.currentThread().interrupt(); process.destroyForcibly(); }
        socket.delete(); control.delete(); policy.delete();
    }
}
