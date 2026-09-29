# Sleep bridge

The system bridge polls the stock PowerManager service every two seconds from
its own handler thread. It uses the bridge's platform privilege and `DUMP`
permission to read PowerManager's live `Wake Locks` section, counts active
locks, and also checks active audio playback and `STAY_ON_WHILE_PLUGGED_IN`
while the machine is charging. It sends one aggregate `set_wake_state` update
over the existing `vendor.matonos.channel.IChannel/sleep` Binder channel.
No per-daemon socket or AOSP patch is used.

At bridge startup, the bridge first asks sleepd to hold suspend until it has a
complete PowerManager snapshot. A failed or incomplete snapshot remains
blocked and is retried. Sleepd refuses suspend while the aggregate blocker is
active and logs the source counts. After all blockers clear, its existing
input-idle policy resumes. Updates expire after ten seconds without a bridge
heartbeat so a dead bridge cannot leave sleep disabled forever. That timeout
is a recovery trade-off: if the bridge process dies while Android still owns
a wake lock, sleepd eventually permits suspend until the bridge restarts.

## Test status

Native compilation, preflight, and the coordinated full image build at
2026-09-28 03:20 succeeded. On a fresh 4 GB headless QEMU boot,
`sys.boot_completed=1`, and PackageManager registered
`org.matonos.systembridge` at
`/system_ext/priv-app/MatonSystemBridge/MatonSystemBridge.apk`.

I used the system shell's PowerManager command to create a genuine system
partial wake lock, avoiding a UI test app because QEMU's software-rendered
launcher is unstable. With `persist.vendor.maton.sleep_idle_s=15`,
`cmd power set-wakelock list` showed `PARTIAL_WAKE_LOCK` held and
`dumpsys power` showed `mWakeLockSummary=0x1`. The bridge observed it and
sleepd deferred idle suspend past the configured timeout. Releasing it made
the bridge forward `blocked=false`; sleepd logged the release and then
`suspending (idle)` after the timeout:

```text
MatonSystemBridge: Forwarded Android wake state: blocked=true wakeLocks=1 audio=false stayAwake=false
matonos-sleepd: Android wake state holds suspend (wake locks=1 audio=inactive stay-awake=disabled)
matonos-sleepd: suspend deferred (idle): Android wake state is active (wake locks=1 audio=inactive stay-awake=disabled)
matonos-sleepd: Android wake state released suspend (wake locks=0 audio=inactive stay-awake=disabled)
MatonSystemBridge: Forwarded Android wake state: blocked=false wakeLocks=0 audio=false stayAwake=false
matonos-sleepd: suspending (idle)
```

QEMU reached the daemon's `suspending (idle)` path; this verifies policy
handoff but is not a real PC suspend/resume hardware test. The earlier provider
parse failure and shared sepolicy failure are resolved in the later successful
coordinated build and are no longer blockers for this test.

## Fresh-boot checks

1. Boot a freshly built image. Set `persist.vendor.maton.sleep_idle_s` to `0`
   during setup, then choose a short positive timeout for the test.
2. On QEMU, acquire a system partial wake lock with
   `cmd power set-wakelock acquire PARTIAL_WAKE_LOCK`. Confirm it appears in
   `cmd power set-wakelock list`; the bridge should report `blocked=true` and a
   nonzero lock count, and sleepd should log `Android wake state holds suspend`.
3. Wait longer than the configured sleep idle timeout. Confirm sleepd logs
   `suspend deferred` and the PC stays awake.
4. Release it with `cmd power set-wakelock release PARTIAL_WAKE_LOCK`. Confirm
   the bridge sends `blocked=false` and
   sleepd logs `released suspend`; after the normal idle interval it should
   suspend.
5. Repeat with audio playback and with Android's stay-awake developer option
   while on AC power. Confirm either condition also blocks suspend and that
   playback/setting removal releases it.
6. On QEMU, set a short idle timeout and verify the bridge/sleepd transitions
   in logcat. Real suspend is not required if QEMU reports unsupported sleep.

## Real hardware

On the Ryzen 5800X/RX 6600 build PC, use a partial-wake-lock test app while
playing audio on AC power, then release each blocker and confirm idle suspend
returns. On Surface Pro 3, repeat while on battery and AC, then separately
verify lid-close and wake behavior after releasing the lock. On HP ProDesk
600 G1, confirm a held lock prevents idle sleep and a keyboard/mouse event
resets the idle timer after the lock is released.

## Open issue

PowerManager's text dump is a privileged diagnostic interface rather than a
stable typed API. The parser is deliberately limited to the named wake-lock
section and fails closed on missing/truncated sections. A future platform
callback would avoid polling, but requires an AOSP change and is excluded by
the zero-patch rule.

Audio playback and the stay-awake setting still need separate end-to-end tests
on a fresh image. No AOSP patch was added.
