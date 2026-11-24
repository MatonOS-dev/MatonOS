# bthal1 implementation report

Implementation commit: `7d4b52356b81b34a22606c8db7fb919ed19a7376`

## Files changed

- `bluetooth/hal/`: forked upstream HAL sources, renamed Android.bp modules,
  init service and VINTF fragment, cancellable MGMT wait, asynchronous setup,
  and this provenance note.
- `bluetooth/bluetooth.mk`: packages the MatonOS HAL binary.
- `sepolicy/vendor/file_contexts`: labels the new binary as
  `hal_bluetooth_default_exec`.
- `bluetooth/README.md`: documents wait/hotplug, restart, and validation
  behavior.

## Threading design

`mLifecycleMutex` serializes `initialize()`, `close()`, and `clientDied()`.
`mStateMutex` protects state and the shared `mCb`, `mFd`, `mH4`, and
`management_` members. The initialization worker owns local fd/H4 values until
it commits them while holding `mStateMutex`. The FD watcher retains its H4
object; watcher callbacks snapshot `mCb` under the state lock and call the
stack after releasing it.

During close or client death, the lifecycle lock is held, state changes to
CLOSING, the management eventfd is signaled, the watcher is stopped, and the
worker is joined. Cleanup then clears shared state and returns to READY. The
worker never takes `mLifecycleMutex`, so close can join it without a lock
cycle. `initializationComplete()` is called without either mutex held.

## Verification

- `clang++ -fsyntax-only` passed for `BluetoothHci.cpp`,
  `net_bluetooth_mgmt.cpp`, and `service.cpp`, using AOSP generated Bluetooth
  headers and source include paths. Host compatibility defines were needed for
  Bionic availability annotations when compiling `BluetoothHci.cpp` against
  host libc headers.
- `git diff --cached --check` passed before the implementation commit.
- No Soong build, AOSP image build, VM boot, or hardware test was run, per the
  task constraints. Runtime cancellation, hotplug, and callback behavior remain
  unverified on-device.

The requested report path under `/mnt/data/aosp/out/` is outside the explicitly
allowed worktree, so this report is committed here instead.
