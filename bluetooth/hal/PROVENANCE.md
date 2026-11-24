# Bluetooth HCI HAL provenance

The source files in this directory are based on AOSP's generic Bluetooth HCI
AIDL HAL at `hardware/interfaces/bluetooth/aidl/default/`, commit
`0162af698935100a590b7359581ac8b1b80693e5`. The upstream files are licensed
under Apache License 2.0; their original copyright and license headers are
retained.

MatonOS changes:

- Renamed the service binary, static library, defaults module, init service,
  and manifest filegroup to the `matonos` names; the service remains
  `IBluetoothHci/default` and uses the upstream vendor service identity,
  user, groups, capabilities, and task profile.
- Added an eventfd control descriptor to MGMT discovery. `cancel()` wakes the
  control-socket poll and produces a `wait cancelled` log; a fresh session
  drains the eventfd before work begins.
- Made `initialize()` start one setup worker and return. The worker performs
  MGMT discovery, the serial fallback, the retained Cuttlefish/goldfish reset,
  H4 setup, and the initialization callback.
- Made `close()` and `clientDied()` serialize against initialize/close,
  cancel an in-progress wait, join the worker, and release shared state. A
  cancellation does not invoke the old initialization callback.
- Protected state and shared callback/fd/H4/management access with
  `mStateMutex`. Callback objects are copied under the lock and called after
  releasing it. The file-descriptor watcher retains its H4 instance, while
  close stops the watcher before clearing shared state.
- Configured one Binder worker thread so close and client death can be
  dispatched during setup. The old unexpected-state path now returns after
  reporting `ALREADY_INITIALIZED`. The stack is the only client, so an
  initialize while a previous setup is still waiting (`INITIALIZING`) or after
  the previous client died cancels and cleans up that session first, then
  starts again; it never waits for hardware.
- The controller wait keeps upstream's 500 ms re-poll and adds the cancel
  eventfd to the same poll.

## Threading and locking

`mLifecycleMutex` serializes `initialize()`, `close()`, and `clientDied()`;
joining never holds `mStateMutex`. The setup worker owns its local fd/H4 until
it commits them under `mStateMutex`. `mStateMutex` protects `mState`, `mCb`,
`mFd`, `mH4`, `management_`, and the dead-client marker. H4 watcher callbacks
copy `mCb` under that mutex and call the stack with no lock held. `close()`
marks CLOSING under the state lock, signals the management eventfd, stops the
watcher, joins the worker, then clears state and closes the active HCI path.
The worker does not acquire `mLifecycleMutex`, so a close can join it without
a lock cycle.

## Verification limits

No Soong build, VM, or device test was run in this task. Host syntax-only
compilation is attempted where the checkout's generated headers permit it;
missing generated headers and resulting unverified interfaces are reported in
the task report.
