# Own D-Bus in Java: concept and plan

Drafted 2026-10-07. Status: **Phases 1–3 and 5 done** in the working tree; the
host C↔GDBus gates are green; **Phase 4 (device validation) pending** a build +
VM test.

> **Phase 1 — codec (done).** `DBusSignature`, `DBusReader`, `DBusWriter`,
> `PortalWire`. Host JVM suite passes; every message type round-trips
> byte-for-byte against dbus-java 5.2.2 used as an independent oracle (method
> calls differ only in non-normative header-field order; same length/content).
>
> **Phase 2 — connection (done).** `PortalChannel`, `PeerConnection`,
> `LocalSocketChannel` (Android), `PipeChannel` (test). `PortalExportedObjects`
> and `MatonLocalTransportProvider` (and their `META-INF/services` entries) are
> deleted; `JavaPortal` runs the own connection. New host test drives a real
> `PeerConnection` over a loopback socket (method call, error, emitted signal,
> inbound signal).
>
> **Phase 3 — dependency removed (done).** The dbus-java/SLF4J jars,
> `libs/README.md`, `libs/SHA256SUMS`, the `implementation files(...)` line and
> the `verifyDbusVendoredJars` task are gone. `grep -rn org.freedesktop.dbus
> app/` is empty and the Android portal sources compile against the SDK
> `android.jar` alone. Fallback if the build/test regresses: restore the deleted
> files from git (`git checkout -- app/libs app/build.gradle`) and revert
> `JavaPortal` to the dbus-java connection.
>
> **Phase 4 — device validation (pending; checklist below).**
>
> **Phase 5 — docs (done).** `NOTES.md`, `linux/dbus-broker/README.md`,
> `CLAUDE.md`.
>
> The C↔GDBus gates (`check-portal-wire`, `check-inhibit`) are **green**
> (2026-10-07, host with `libglib2.0-dev`): real GDBus blobs round-trip through
> the Java codec (little/big-endian, nested `a{sa{sv}}`, the `(ddd)` accent
> tuple, error names, `SCM_RIGHTS`+`CLOEXEC`, fragmentation, malformed frames)
> and the inhibit/monitor/session/disconnect lifecycle passes. They still spawn
> the unchanged `PortalTestPeer`.



This supersedes the "dbus-java 6.x is the eventual target" note
(`HANDOVER-2026-10-05-late.md`, `NOTES.md` line 1736). The goal is to remove
the vendored `dbus-java`/SLF4J jars from the compositor and replace them with a
small, in-tree, pure-Java D-Bus implementation covering exactly the subset the
portal uses.

## 1. Why

- **Launch blocker.** dbus-java 5.2.2's `SASL.<clinit>` calls
  `Collator.setDecomposition(FULL_DECOMPOSITION)`; Android ICU throws
  "Bad mode: 2", the compositor's `java-portals` thread dies and the Flatpak
  app reports "Cannot open application session"
  (`HANDOVER-2026-10-07-afternoon.md`).
- **Not ours.** The dependency is a vendored jar we do not control
  (`linux/compositor/host/app/libs/dbus-java-core-5.2.2.jar` + `slf4j-api` +
  `slf4j-nop`), pinned to a release whose successor is unreleased and whose
  only "fix" would be a fork.
- **Principle fit.** `CLAUDE.md`: prefer our own components around Android,
  and our own code (including the compositor and the D-Bus broker) lives in
  this one repository. dbus-java is the one third-party dependency left in the
  portal path.
- **We already rewrote the rest.** Framing, authentication, fd passing, the
  header walk, the signature parser, the message model, routing, policy and
  introspection are all already ours (see §3). The only thing dbus-java still
  supplies is a value codec, a header codec and a connection/dispatch shell.

### Alternatives considered

| Option | Verdict |
| --- | --- |
| Patch/fork dbus-java | Hours, but keeps the jar, SLF4J, the fake SASL and the dependency; the forks rule wants a real fork; 6.x is unreleased. Not "something we control". |
| JNI binding to GDBus | GDBus supplies codec/auth/dispatch for free, but reintroduces a native↔Java boundary (JNI + a GVariant↔Java bridge + `GMainLoop` integration), loses the plain-JVM test loop, and — because the portal *semantics* need Android Java APIs — is comparable-or-more work. GDBus is the right tool where the consumer is C; it already is, in `linux/dbus-broker`. |
| **Own pure-Java codec + connection** | **Chosen.** ~900–1,200 LOC, no native boundary, keeps the plain-JVM tests, removes the dependency and the SASL stub. |

## 2. Boundaries / non-goals

- **Do not touch the native broker** (`linux/dbus-broker/`). It is the session
  bus the apps talk to and it stays. The MBP1 forward protocol and the shared
  secret handshake are unchanged, so broker compatibility is preserved.
- **Do not touch portal semantics/policy**: `PortalBackend` (routing, caller
  ownership, inhibit lifetime) and `PortalIntrospection` are already
  dbus-java-free and stay as they are.
- **Do not build a general D-Bus library.** No bus daemon, name registry,
  match rules, activatable services, introspection generation, or generic
  object model. The portal is one authenticated peer-to-peer connection with a
  fixed set of interfaces.
- No new native code, no new Android permissions, no SELinux changes.

## 3. What already exists (why this is small)

| Layer | Where | Status |
| --- | --- | --- |
| MBP1 framing (8-byte LE length / fd-count) | `PortalWire.validateFrameHeader`, `MatonLocalTransportProvider.PortalSocketChannel`, broker `portals.c` | hand-written, ours |
| Auth (MBP1 magic + 64-byte secret) | `JavaPortal.serve`, `MATON_PORTAL_SECRET` | ours |
| "SASL" | `MatonLocalTransportProvider.authReply` | stub; its own comment says the lines are **never sent to the broker**. Deletable with no loss. |
| fd passing (`SCM_RIGHTS`) | `PortalSocketChannel` + `LocalSocket` get/setFileDescriptorsForSend / getAncillaryFileDescriptors | hand-written, ours |
| Header field walk + `UNIX_FDS` count | `PortalWire.fdCount` | hand-written |
| Signature parser | `PortalWire.signatureEnd/signaturePart`, `PortalExportedObjects.signatureEnd` | hand-written |
| Message model | `PortalWire.Message` (type/flags/serial/…/body) | ours; the two dbus-java fields (`dbusMessage`, `replyTo`) were removed in Phase 2 |
| Routing, policy, state | `PortalBackend` | ours, dbus-java-free |
| Introspection XML | `PortalIntrospection` | ours |
| Caller identity | broker sets `sender` to the app's unique name; `PortalBackend` keys handles by sender | ours |
| Bus + policy + routing | `broker.c` (GDBus, 1,061 lines) | native, stays |

So dbus-java contributes only: **value marshalling**, **header marshalling**,
and a **connection/object-dispatch shell** — plus a SASL state machine we do
not want.

An important consequence: `PortalExportedObjects` exists only because
dbus-java needs annotated interfaces; `PortalBackend.invoke` already routes by
path/interface/member (including `Request`/`Session` `Close`, `Properties`,
and `Introspectable`). The object layer therefore collapses to
"decode a method call → `backend.dispatch` → encode the replies/signals".

## 4. Wire subset to implement

**Header fields** (id, type): PATH `1 o`, INTERFACE `2 s`, MEMBER `3 s`,
ERROR_NAME `4 s`, REPLY_SERIAL `5 u`, DESTINATION `6 s`, SENDER `7 s`,
SIGNATURE `8 g`, UNIX_FDS `9 u`. The header is `yyyyuua(yv)`, protocol
version 1, padded to 8.

**Body types** on the wire (from `PortalBackend`,
`PortalExportedObjects`, broker signals): `y b n q i u x t d s o g v h`, arrays
`a`, dicts `a{sv}` and `a{sa{sv}}`, and the `(ddd)` accent-color struct.
Implement the full basic set; only these appear.

**Rules:** little-endian on x86_64, **decoder must also accept big-endian**
(`portal-wire-test.c` sends both); alignment 1/2/4/8; a variant is a 1-byte
signature followed by its value; `h` is a 0-based index into the message's
`UNIX_FDS` list; arrays carry a byte length and align their element.

**Limits** (match the C and Java sides today): `MAX_MESSAGE` 1 MiB,
`MAX_FDS` 16, `MAX_CALLS` 256, plus a recursion-depth cap on nested
containers and strict bounds checks on every read.

**Direction:** `fd`s arrive from the broker for `OpenFile`/`OpenDirectory`
(the ported `portal-wire-test.c` case), so only *incoming* fd handling is
load-bearing; replies and signals carry none. Support the outgoing path too
for symmetry (the existing `Writer` already does).

## 5. Target architecture

```
              app / Flatpak  ──GDBus──►  native broker  ──MBP1 frames──┐
              (unique name)              (linux/dbus-broker)          │
                                                                      ▼
   CompositorService ──► SessionBus ──► JavaPortal  ──► PeerConnection ──► PortalChannel
                                              │              │
                                              ▼              ▼
                                        PortalBackend   org.matonos.dbus codec
                                        PortalFiles     (signature/reader/writer/message)
                                        PortalIntrospection
```

New code (no Android imports, so it compiles and runs on a stock JVM):

| Class | Responsibility | Est. LOC |
| --- | --- | --- |
| `DBusSignature` | parse/validate signatures, iterate types, element/dict helpers | 120 |
| `DBusReader` | alignment + bounds-checked read of all body types, variant recursion, fd index | 200 |
| `DBusWriter` | mirrored writer (LE), containers, variant, fd index | 180 |
| `DBusError` | error name + message mapping | 30 |
| `PeerConnection` | blocking read loop; serials; dispatch incoming method call → handler; encode/send replies and signals; `ClientClosed` handling; close/cleanup | 250 |
| `PortalChannel` (interface) | framed read/write + fd transfer, transport-agnostic | 40 |
| `LocalSocketChannel` (Android) | the framing + `SCM_RIGHTS` core salvaged from `PortalSocketChannel`, minus the `SocketChannel` facade and fake SASL | 140 |
| `PipeChannel` (test source set) | the same interface over `InputStream`/`OutputStream`, replacing `PortalTestPeer`'s inline framing | 60 |

Reused/reduced:

| Class | Change |
| --- | --- |
| `PortalWire` | keep as the public model/facade (`Message`, `Variant`, `encode`, `decode`, `validateFrameHeader`, `dictionary`); reimplement its internals on the new codec; drop the dbus-java fields/imports |
| `PortalExportedObjects` | collapse to a ~40-line router (`backend.dispatch` + send); no annotated interfaces, no annotations |
| `JavaPortal` | keep MBP1 handshake, fd ownership and platform callbacks; swap `DirectConnectionBuilder`/`DirectConnection` for `LocalSocketChannel` + `PeerConnection` |
| `MatonLocalTransportProvider` | **delete** (transport provider, socket provider, reader/writer, `SocketChannel` subclass, fake SASL all go) |
| `SessionBus`, `PortalBackend`, `PortalIntrospection`, `PortalFiles`, `CompositorService` | unchanged |

Descriptor ownership: `PortalChannel.read` acquires a frame's descriptors and
`PeerConnection` releases them via `PortalChannel.closeDescriptors`, once per
frame, whatever the outcome — so received fds are disposed of in exactly one
place, not by a distant handler.

## 6. Migration strategy

1. **Keep `PortalWire`'s public API** so `PortalBackend`, `PortalTestPeer` and
   `PortalBackendTest` compile unchanged; only the two dbus-java references in
   `PortalBackendTest` (`m.dbusMessage.getFiledescriptors()`,
   `new org.freedesktop.dbus.FileDescriptor(...)`) are retouched.
2. **Make `PeerConnection` transport-agnostic** so the host JVM suite can drive
   the real connection over pipes (as `PortalTestPeer` does today) while the
   app uses `LocalSocketChannel`.
3. **Land in phases**, each gated by the existing tests (§8), so there is
   always a working fallback (the jar stays until Phase 4 passes).

## 7. Phases and work breakdown

Assumes one focused engineer/agent; times are working days.

Status as executed (2026-10-07): Phases 1–3 and 5 are done in the working tree;
Phase 4 (device validation) is pending. Deviations from the plan are noted.

### Phase 0 — Baseline (done)
- Froze the signature set (§4) and the `MAX_*` constants.
- `make -C linux/dbus-broker check-portal-wire check-inhibit` is green after the
  rewrite (2026-10-07).

### Phase 1 — Codec (done)
- Added `DBusSignature`, `DBusReader`, `DBusWriter` (no separate `DBusError`;
  bad input throws `IllegalArgumentException`).
- Reimplemented `PortalWire.encode/decode/validateFrameHeader/declaredFdCount`
  on them and removed every dbus-java import. The `AccentColor` struct became a
  plain `List<Double>`.
- **Acceptance:** host `test-portals.sh` green; oracle byte-compat green; the
  C↔GDBus gates green (run 2026-10-07 on a host with `libglib2.0-dev`).

### Phase 2 — Connection and routing (done)
- Added `PortalChannel`, `PeerConnection`, `LocalSocketChannel`, `PipeChannel`.
- Deleted `PortalExportedObjects` and `MatonLocalTransportProvider` outright
  (they existed only for dbus-java); `JavaPortal.run()` now uses
  `LocalSocketChannel` + `PeerConnection`, and an inner handler calls
  `backend.dispatch` and routes `ClientClosed`.
- **Deviation:** instead of extending `PortalTestPeer`, added a separate
  `PortalConnectionTest` driving a real `PeerConnection` over a loopback
  socket, leaving the C harness (`PortalTestPeer`) untouched and giving the
  connection its own JVM test.
- **Acceptance:** host `test-portals.sh` green (backend + connection); Android
  portal sources compile against the SDK `android.jar` alone.

### Phase 3 — Remove the dependency (done)
- Deleted the three jars, `libs/README.md`, `libs/SHA256SUMS`, and the
  `implementation files(...)` line + `verifyDbusVendoredJars` task.
- **Acceptance:** `grep -rn org.freedesktop.dbus app/` empty; portal sources
  compile with `android.jar` alone; `tools/build-apps.sh` stages
  `MatonWaylandHost.apk` whose `classes.dex` has no `freedesktop/dbus`,
  `hypfvieh` or `slf4j` strings (checked 2026-10-07).

### Phase 4 — Device validation (pending; run after the build)
Recipe: copy the image, set the ESP `matonos-debug-permissive.conf` default,
boot with `tools/run-qemu-live.sh -g std`, `setprop
persist.vendor.maton.sleep_idle_s 0`, open the store, install and launch a
Flatpak.
- [x] `tools/build-apps.sh` stages `MatonWaylandHost.apk` with no dbus-java in
  the dex (2026-10-07).
- [ ] Image build (`MATON_BUILD_COORDINATOR=1 tools/build.sh -K -M -j 16`).
- [ ] Flatpak launches (the `SASL.<clinit>` crash is gone).
- [ ] Settings: `Read`/`ReadOne`/`ReadAll` (color-scheme, contrast, accent).
- [ ] `OpenURI` and `OpenFile`/`OpenDirectory` (fd capability, read/write flags).
- [ ] `Inhibit` hold (wake lock), `CreateMonitor`, `QueryEndResponse`, `Session.Close`.
- [ ] `ClientClosed` on app exit mid-hold (file owners released, hold drops).
- [ ] Several sessions at once; no leaked fds over a launch/kill cycle.

### Phase 5 — Cleanup and docs (done)
- `NOTES.md` decision bullet, `linux/dbus-broker/README.md` (transitional note
  and transport section), `CLAUDE.md` library list.
- `PortalWire` is kept as the message-model/codec class; no rename needed.

**Total: ~5–8 days**, ~900–1,200 LOC new/reworked.

## 8. Test plan

| Gate | What it proves | Where |
| --- | --- | --- |
| Host JVM unit (`test-portals.sh` → `PortalBackendTest`) | codec round-trips, variants/arrays, caller ownership, inhibit lifetime, monitors, malformed frame/message lengths | plain JVM, no Android |
| Host JVM integration (`PortalConnectionTest`) | the real `PeerConnection` dispatch path over a loopback socket, plus emitted and inbound signals | plain JVM |
| **GDBus cross-language** (`make check-portal-wire`) | byte compatibility: LE+BE, nested `a{sa{sv}}`/accent `(ddd)`, error names, `SCM_RIGHTS`+`CLOEXEC`, fragmentation, malformed frames | C ↔ Java |
| **GDBus lifecycle** (`make check-inhibit`) | inhibit aggregation/ownership/disconnect/monitor/transport-death through the real broker with real GDBus clients | C ↔ Java |
| Instrumented (`MatonLocalTransportInstrumentedTest`) | `LocalSocket` peer uid and one `SCM_RIGHTS` descriptor on-device | device |
| Device/VM | end to end in the image | VM |

The two bolded C tests are the strongest guard: they already send GDBus blobs
straight into `PortalWire.decode` and feed `PortalWire.encode` back to GDBus,
so they pin the value and header codecs byte for byte.

## 9. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Alignment/variant/header byte-exactness | `check-portal-wire` covers LE+BE, nesting, dicts, tuple, errors; add golden vectors from captured blobs |
| `UNIX_FDS` index / fd lifetime bugs | `portal-wire-test.c` already passes an fd and asserts `CLOEXEC`; add device fd-leak cycle test |
| Partial/fragmented frames | channel buffers as today; fragmentation case already in `portal-wire-test.c` |
| Recursion / oversize / malformed input | depth cap, size limits, strict bounds checks; existing malformed-frame cases |
| Threading | unchanged single-thread blocked-read model; no `GMainLoop`/callback boundary (the main reason this beats the GDBus-JNI route) |
| `NO_REPLY_EXPECTED` calls | still reply; the broker drops pending entries, so an extra reply is harmless — assert this in a host test |
| Serial wrap/collision | `PortalBackend` already guards `serial == 0`; keep serials per-connection |
| Reply routing (outbound calls) | the portal initiates no calls today; implement minimal `replySerial` routing for completeness, but it is not load-bearing |
| Test-only pipe code shipped | `PipeChannel` lives in the test source set only |

## 10. Rollout and fallback

Work on a normal branch; land Phases 1–3 behind the existing tests, validate on
the VM in Phase 4, then delete the jars in a final commit (or keep the delete
last). If Phase 4 regresses, reverting the Phase 2 connection swap restores
dbus-java 5.2.2 immediately; the codec and connection are additive until then.

## 11. Open decisions

1. Package name: `org.matonos.dbus` (recommended, public, Android-free) vs
   keeping the codec inside `org.matonos.compositor`.
2. Big-endian **encode**: not needed (only x86_64). Decode must support it.
3. Retire the `PortalWire` facade now or after Phase 5.
4. Confirm the native broker retirement (the Flatpak-stub handover's next
   step 6) is out of scope here — this plan only removes the Java library.
