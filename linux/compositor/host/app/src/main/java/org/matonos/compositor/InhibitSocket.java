package org.matonos.compositor;

import android.content.Context;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.system.Os;
import android.util.Log;
import java.io.File;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CountDownLatch;

/** Private host-side session transport. One byte is the
 * broker's aggregate hold state; echo acknowledges the applied Android hold.
 * EOF (including broker death) releases it. No app IDs or global bridge API. */
final class InhibitSocket implements AutoCloseable {
    private final LocalSocket bound = new LocalSocket();
    private final LocalServerSocket server;
    private final Set<LocalSocket> clients = new HashSet<>();
    private final Set<LocalSocket> holders = new HashSet<>();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final PowerManager.WakeLock wake;
    private final java.util.function.Consumer<Boolean> changed;
    private final File path;
    private volatile boolean stopped, held;

    InhibitSocket(Context context, File directory, java.util.function.Consumer<Boolean> changed) throws Exception {
        this.changed = changed;
        path = new File(directory, "inhibit");
        if (path.exists() && !path.delete()) throw new java.io.IOException("Cannot remove old inhibit socket");
        wake = context.getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MatonOS:FlatpakInhibit");
        wake.setReferenceCounted(false);
        bound.bind(new LocalSocketAddress(path.getAbsolutePath(), LocalSocketAddress.Namespace.FILESYSTEM));
        // Only the compositor-owned broker can acquire a hold.
        Os.chmod(path.getAbsolutePath(), 0600);
        server = new LocalServerSocket(bound.getFileDescriptor());
        new Thread(this::accept, "flatpak-inhibit-accept").start();
    }
    boolean isHeld() { return held; }
    private void accept() {
        while (!stopped) {
            try {
                LocalSocket socket = server.accept();
                synchronized (clients) {
                    if (stopped || socket.getPeerCredentials().getUid() != android.os.Process.myUid() || clients.size() >= 16) {
                        socket.close(); continue;
                    }
                    clients.add(socket);
                }
                new Thread(() -> read(socket), "flatpak-inhibit").start();
            } catch (Exception e) { if (!stopped) Log.w("MatonInhibit", "Accept failed", e); break; }
        }
    }
    private void apply(LocalSocket socket, boolean active) throws InterruptedException {
        CountDownLatch applied = new CountDownLatch(1);
        main.post(() -> {
            try {
                if (active && !stopped) holders.add(socket); else holders.remove(socket);
                boolean next = !holders.isEmpty();
                if (next != held) {
                    if (next) wake.acquire(); else if (wake.isHeld()) wake.release();
                    held = next;
                    changed.accept(next);
                }
            } catch(RuntimeException e) { Log.e("MatonInhibit", "Cannot apply Android hold",e); }
            finally { applied.countDown(); }
        });
        applied.await();
    }
    private void read(LocalSocket socket) {
        try {
            for (;;) {
                int value = socket.getInputStream().read();
                if (value != 0 && value != 1) break;
                apply(socket, value == 1);
                // If wake acquisition failed, do not acknowledge success.
                if (value == 1 && !held) break;
                socket.getOutputStream().write(value);
            }
        } catch (Exception e) { if (!stopped) Log.w("MatonInhibit", "Broker disconnected", e); }
        finally {
            try { apply(socket, false); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            synchronized (clients) { clients.remove(socket); }
            try { socket.close(); } catch (Exception ignored) { }
        }
    }
    @Override public void close() {
        stopped = true;
        try { server.close(); bound.close(); } catch (Exception ignored) { }
        synchronized (clients) { for (LocalSocket socket : clients) try { socket.close(); } catch (Exception ignored) { } }
        // Service destruction runs on main; release immediately, then EOF cleanup is harmless.
        holders.clear();
        if (wake.isHeld()) wake.release();
        if (held) { held = false; changed.accept(false); }
        path.delete();
    }
}
