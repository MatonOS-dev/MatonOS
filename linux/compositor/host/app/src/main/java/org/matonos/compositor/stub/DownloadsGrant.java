package org.matonos.compositor.stub;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.IBinder;
import android.os.Parcel;
import org.json.JSONObject;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.charset.CodingErrorAction;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

/** Foreground consent belongs to the signed stub, never the Linux payload. */
final class DownloadsGrant {
    private volatile boolean closed;
    private volatile CountDownLatch pending;
    private AlertDialog dialog;

    private static IBinder linuxd() throws Exception {
        // Same self-only service access as StubService.prepareRuntimeDirectory.
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("checkService", String.class)
                .invoke(null, "org.matonos.systembridge.ILinuxd/default");
        if (binder == null) throw new IllegalStateException("Linux service unavailable");
        return binder;
    }

    /* Append-only transactions 8/9 in install/linuxd/aidl/.../ILinuxd.aidl.
     * The host's existing Java AIDL copy intentionally stays unchanged in
     * this scoped change. Keep these IDs synchronized (host protocol test).
     * This uses public Binder APIs, with the same interface token and reply
     * encoding as the generated proxy; linuxd still verifies the real UID. */
    private static String transact(IBinder daemon, boolean answer, String packageName,
            String request, boolean allow) throws Exception {
        Parcel input = Parcel.obtain();
        Parcel output = Parcel.obtain();
        try {
            input.writeInterfaceToken("org.matonos.systembridge.ILinuxd");
            input.writeString(packageName);
            if (answer) { input.writeString(request); input.writeBoolean(allow); }
            if (!daemon.transact(IBinder.FIRST_CALL_TRANSACTION + (answer ? 9 : 8), input, output, 0))
                throw new IllegalStateException("Update MatonOS for folder permissions");
            output.readException();
            String result = output.readString();
            if (result == null) throw new IllegalStateException("Empty folder response");
            return result;
        } finally { input.recycle(); output.recycle(); }
    }

    private static Map<String, Boolean> requests(String wire) throws Exception {
        Map<String, Boolean> requests = new LinkedHashMap<>();
        if (wire.isEmpty()) return requests;
        if (!wire.endsWith("\n")) throw new IllegalArgumentException("Incomplete folder request");
        for (String line : wire.split("\n")) {
            String[] fields = line.split(":", -1);
            if (fields.length != 3 || !fields[0].equals("-1")
                    || !(fields[1].equals("0") || fields[1].equals("1"))
                    || fields[2].length() % 2 != 0)
                throw new IllegalArgumentException("Invalid folder request");
            byte[] bytes = new byte[fields[2].length() / 2];
            for (int i = 0; i < bytes.length; i++) {
                int a = Character.digit(fields[2].charAt(i * 2), 16);
                int b = Character.digit(fields[2].charAt(i * 2 + 1), 16);
                if (a < 0 || b < 0 || (a == 0 && b == 0))
                    throw new IllegalArgumentException("Invalid folder path");
                bytes[i] = (byte) (a * 16 + b);
            }
            String path = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes)).toString();
            if (requests.put(path, fields[1].equals("1")) != null)
                throw new IllegalArgumentException("Duplicate folder path");
        }
        return requests;
    }

    /** A smaller request retains its answer; more paths or write access asks again. */
    private static boolean covered(Map<String, Boolean> current, Map<String, Boolean> asked) {
        for (Map.Entry<String, Boolean> request : current.entrySet()) {
            boolean found = false;
            for (Map.Entry<String, Boolean> prior : asked.entrySet()) {
                String path = prior.getKey();
                if ((path.isEmpty() || request.getKey().equals(path)
                        || request.getKey().startsWith(path + "/"))
                        && (request.getValue() || !prior.getValue())) { found = true; break; }
            }
            if (!found) return false;
        }
        return true;
    }

    /** Called on the launch worker; only the dialog runs on the Activity thread. */
    void beforeLaunch(Activity activity, BooleanSupplier foreground) throws Exception {
        IBinder daemon = linuxd();
        JSONObject response = new JSONObject(transact(daemon, false, activity.getPackageName(), "", false));
        if (!response.optBoolean("ok"))
            throw new IllegalStateException(response.optString("error", "Cannot read folder permissions"));
        String request = response.getString("output");
        Map<String, Boolean> current = requests(request);
        SharedPreferences state = activity.getSharedPreferences("flatpak_folder_grants", Activity.MODE_PRIVATE);
        // Stored with ';' for newlines: SharedPreferences XML does not keep
        // trailing whitespace intact. Request lines never contain ';'.
        String stored = state.getString("downloads_request", null);
        String asked = stored == null ? null : stored.trim().replace(";", "\n").trim();
        if (asked != null && !asked.isEmpty()) asked += "\n";
        boolean answered = asked != null && covered(current, requests(asked));
        boolean allow = answered && state.getBoolean("downloads_allow", false) && foreground.getAsBoolean();
        if (!current.isEmpty() && !answered && !closed) {
            boolean readonly = true;
            for (boolean ro : current.values()) readonly &= ro;
            final boolean readOnly = readonly;
            final boolean[] choice = {false};
            final AtomicBoolean decided = new AtomicBoolean();
            CountDownLatch done = new CountDownLatch(1);
            pending = done;
            activity.runOnUiThread(() -> {
                if (closed || !foreground.getAsBoolean()) { done.countDown(); return; }
                CharSequence label = activity.getApplicationInfo().loadLabel(activity.getPackageManager());
                String message = readOnly ? "Allow " + label + " to read your Downloads folder?"
                        : "Allow " + label + " to access your Downloads folder?";
                java.util.function.Consumer<Boolean> answer = value -> {
                    if (decided.compareAndSet(false, true)) {
                        if (!closed && foreground.getAsBoolean()) {
                            choice[0] = value;
                            state.edit().putString("downloads_request", request.replace("\n", ";"))
                                    .putBoolean("downloads_allow", value).commit();
                        }
                        done.countDown();
                    }
                };
                dialog = new AlertDialog.Builder(activity).setMessage(message)
                        .setPositiveButton("Allow", (d, which) -> answer.accept(true))
                        .setNegativeButton("Deny", (d, which) -> answer.accept(false))
                        .setCancelable(false).create();
                dialog.setOnDismissListener(d -> answer.accept(false));
                dialog.show();
            });
            done.await(); pending = null; allow = choice[0];
        }
        // An unanswered background request is denied without persisting it.
        if (closed) return;
        allow &= foreground.getAsBoolean();
        JSONObject applied = new JSONObject(transact(daemon, true, activity.getPackageName(), request, allow));
        if (!applied.optBoolean("ok"))
            throw new IllegalStateException(applied.optString("error", "Folder permissions changed"));
    }

    void close() {
        closed = true;
        if (dialog != null) dialog.dismiss();
        CountDownLatch done = pending;
        if (done != null) done.countDown();
    }
    void backgrounded() {
        if (dialog != null) dialog.dismiss();
        CountDownLatch done = pending;
        if (done != null) done.countDown();
    }
}
