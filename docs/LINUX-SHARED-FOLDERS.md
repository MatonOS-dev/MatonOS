# Linux apps and Android shared folders

Status: Downloads folder grants implemented in source, 2026-10-11. Host tests
and NDK r30 strict C compile checks pass; no device or enforcing-policy
validation yet. Other folder grants, FileChooser and settings revocation remain
design only.

Linux apps (Flatpaks) see only their own home today. This document defines how
they reach the user's Android shared storage (Download, Documents, Pictures,
Music, Movies) without blanket access.

Two mechanisms, used together:

1. **Folder grants**: a Flatpak that declares a folder permission gets it bound
   into its sandbox after the user allows it once.
2. **File-chooser portal**: any app may ask the user to pick files through
   Android's picker; only the picked files become visible to it.

Rejected: a FUSE layer that prompts on first access to an undeclared folder.
It puts every file operation through userspace and needs /dev/fuse in the app
domain; option 2 already covers undeclared apps that use a portal-aware
toolkit.

## 1. Folder grants

### Source of the request

Flatpak metadata (`[Context] filesystems=`), after user/system overrides, is
the only input. Target mapping (only Downloads is implemented):

| Flatpak filesystem      | Android folder (user `<u>`)        | In sandbox          |
|-------------------------|------------------------------------|---------------------|
| `xdg-download[:ro]`     | `Download`                         | `~/Downloads`       |
| `xdg-documents[:ro]`    | `Documents`                        | `~/Documents`       |
| `xdg-pictures[:ro]`     | `Pictures` + `DCIM`                | `~/Pictures`, `~/DCIM` |
| `xdg-music[:ro]`        | `Music`                            | `~/Music`           |
| `xdg-videos[:ro]`       | `Movies`                           | `~/Videos`          |
| `home`, `host`          | none of the above automatically    | see below           |
| `xdg-download/<sub>` etc. | the subfolder only               | same subpath        |

`home`/`host` stay suppressed (`--nofilesystem=host`, flatpak-env-wrapper.c).
Apps declaring them get the five-folder prompt as a single "Allow access to your
files" grant covering all five, never Android's root or other apps' folders.
Paths outside these roots (`/media`, `/mnt`, arbitrary absolute paths) are not
granted; removable media is a later, separate design.

`:ro` → read-only bind. No suffix (or `:rw`/`:create`) → read-write.

### Prompt

- Shown by the app's stub before the first launch that needs it (and again if
  the metadata's request grows after an update). One dialog listing the folders,
  Allow / Deny.
- The answer is stored per app per Android user in the stub's own state
  (survives app updates, cleared with app data / uninstall). Changeable later
  from the app's settings page ("Files and folders").
- Deny → the app still starts; it gets empty private `~/Downloads` etc. inside
  its home so it does not fail on missing directories.
- Never prompt from a background launch; launch then proceeds as denied until
  the user answers in the foreground.

### Mount

- linuxd (privileged side) resolves the folder from the Android user's storage
  under `/data/media/<u>/<Folder>` and hands an O_PATH fd, not a pathname, to the
  session setup; `matonos-bwrap` binds it at the in-sandbox path
  (`--bind-fd`/`--ro-bind-fd`) and exports `XDG_*_DIR` via a generated
  the app's `.var/app/<id>/config/user-dirs.dirs` (the sandbox's
  `$XDG_CONFIG_HOME/user-dirs.dirs`).
- Going below Android's FUSE (MediaProvider) is deliberate: through FUSE the
  stub UID could only read files it created itself unless it held "All files
  access", which is far broader than one folder.
- Ownership/permissions: files under `/data/media` carry the media_rw group and
  sdcardfs-style project IDs. The bind keeps the lower filesystem's ownership;
  the sandbox UID must be able to create files there (group `media_rw`
  membership only for granted sessions; `ext_data_rw` is specific to
  Android/data and is not used). Quota project IDs must stay correct for new files so
  Android's storage accounting does not attribute them wrongly.
- MediaProvider indexing: files written below FUSE are not seen until a scan.
  On session teardown (and on file close events if cheap) linuxd asks
  MediaProvider to scan the granted folders' changed paths
  (`MediaStore.scanFile`-equivalent through the system bridge).
- SELinux: a dedicated label/allow for the app domain on the granted media
  dirs only; neverallow access to the rest of `media_rw_data_file`.

## 2. File-chooser portal

- Implement `org.freedesktop.portal.FileChooser` (`OpenFile`, `SaveFile`,
  `SaveFiles`) in the Java portal backend next to Settings/Inhibit/OpenURI
  (`PortalBackend.java`).
- `OpenFile` → `ACTION_OPEN_DOCUMENT` (multiple/directory per options; filters
  map to MIME types); `SaveFile` → `ACTION_CREATE_DOCUMENT` with the suggested
  name. Activities are launched from the app's own stub so the picker belongs
  to the app's task.
- Exposing the result: Flatpak normally uses the document portal (FUSE at
  `/run/user/<uid>/doc/<id>/<name>`); we launch with `--no-documents-portal`.
  Replacement: the stub keeps the picked `ParcelFileDescriptor`s and serves them
  through a small per-app document FUSE mount inside the sandbox at the same
  path layout, returning `file:///run/user/<uid>/doc/<id>/<name>` URIs (apps
  expect paths). Only picked documents appear; the mount lives with the session.
  Same mechanism serves `org.freedesktop.portal.Documents` for drag-and-drop
  later.
- Write-back for `SaveFile` goes to the SAF document through the held fd.
- Toolkit coverage: GTK (with `GTK_USE_PORTAL=1`/portal-default), Qt
  (`QT_QPA_PLATFORMTHEME=xdgdesktopportal` or Flatpak default), Chromium and
  Electron use the portal inside Flatpak; apps with their own dialogs see only
  their sandbox and any granted folders.

## Order of work

1. Folder grants for `xdg-download` only, end to end (prompt, fd bind,
   user-dirs, scan, SELinux), then the other folders.
2. FileChooser `OpenFile` with document FUSE (read-only), then `SaveFile`.
3. Settings page entry for revoking grants.

## Open questions

- Verify the implemented Downloads ownership/project inheritance and scan
  behaviour on a device, including secondary users and enforcing SELinux.
- The static-musl staging script has an explicit header copy list. Before a
  future restage it must also copy `DownloadsGrant.h` and
  `DownloadsUserDirs.h` into `helpers/install/linuxd/`. That script is outside
  this brief's C-source edit scope and was not changed or run.
- Whether `PortalFiles`' 256-entry cap (PortalFiles.java) should go: it is an
  arbitrary limit; the document store replacing it should not have one.

## Downloads implementation (2026-10-11)

Only the `xdg-download` mapping is active. Its Flatpak, Android and sandbox
spellings live together in `DownloadsGrant.h`; other mappings above remain
planned. In this first step `home`/`host` request Downloads only. `:ro` is a
read-only bind; absent suffix, `:rw` and `:create` are writable. Subpaths are
validated component by component; absolute paths, `.` and `..`, empty
components and symlink traversal are rejected. Request lists and transport
buffers grow dynamically with overflow checks.

The signed stub asks linuxd for `flatpak --user info --show-permissions` using
the verified installation/runtime pair. This is Flatpak's effective context,
including deployment overrides. Both Flatpak's terminal escaping and GKeyFile
list escaping are decoded. Consent stores the canonical request and answer in
stub-private SharedPreferences, naturally separate for each Android user. New
paths or increased write access require another answer; smaller requests retain
the answer. An unanswered background launch neither prompts nor stores a denial;
background launches receive no bind, even with a previous Allow.

Two self-only native ILinuxd methods are appended after the existing methods.
The stub uses public Parcel/IBinder transactions 8 and 9 because the host's Java
AIDL mirror is outside this change's allowed paths. Linuxd verifies the real
Binder UID/PID and stub identity, re-reads permissions when accepting the answer
and when launching, and consumes a one-launch answer tied to the same pinned
process cgroup. The existing launch route through compositor/System Bridge is
unchanged; a missing or stale answer gives no grant. Stub strings never select
source paths.

Linuxd opens `/data/media/<uid / 100000>/Download[/sub]` with fd-relative
`O_PATH|O_DIRECTORY|O_NOFOLLOW` walks. It does not create the Android user's
storage root. Descriptors are allocated above the existing reserved launch
slots and retained by the linuxd session reaper. `posix_spawn` passes them to
flatpak-env-wrapper, which keeps them CLOEXEC for auxiliary and portal execs,
then makes them inheritable only for foreground `flatpak run`. Flatpak retains
those inherited descriptors and carries `MATON_DOWNLOAD_GRANTS` through its
bundled `--args` environment entries. Matonos-bwrap validates the descriptors
and adds bind-fd/ro-bind-fd mounts after those entries. Bubblewrap consumes and
closes the raw handles before the payload runs; the metadata is unset in the
payload. The existing home bind stays at argv index 1.

Both granted and denied launches prepare private `~/Downloads` and update
`.var/app/<id>/config/user-dirs.dirs`, preserving unrelated lines. The shared
mount covers that private directory only during a granted session.

The lower media storage is media_rw-owned (1023), with default media_rw ACLs;
`ext_data_rw` is specific to Android/data and is never granted here. Linuxd
uses per-thread filesystem UID = stub UID and filesystem GID = media_rw for
source operations, rather than changing the Binder process's supplementary
groups. Only granted app sessions add supplementary media_rw; portal sessions
do not. All payload capabilities are dropped. FOWNER is added to linuxd alone
for mode changes; no DAC_OVERRIDE is added.

New shared directories are media_rw:media_rw, mode 02775. At launch only the
granted directory itself is prepared (label, group media_rw + setgid, project);
its contents are not walked, so launch time does not depend on the size of the
user's Downloads. Existing directories retain their owner and permission bits;
directories created by this writer are normalized to media_rw:media_rw
02775 at teardown. Directory project IDs use Android's per-user default media project
`user * 100000 + 1000` plus PROJINHERIT. Files created by the app initially have
its UID and inherited media group/project. At writable-session teardown linuxd
normalizes regular files owned by that writer to media_rw:media_rw 0664, using
pinned descriptors; other existing file owners are preserved. Traversal is
iterative and rejects cross-filesystem descendants. MediaProvider's scan
subsequently refines audio/video/image project attribution to 1001/1002/1003.
Filesystems without FSGETXATTR support skip project setup; other quota errors
fail the granted launch rather than silently using incorrect attribution.

Download keeps stock Android's `media_rw_data_file` label (decided
2026-10-11). A dedicated type was tried and rejected: stock
`app_neverallows.te` forbids untrusted app domains, MediaProvider included,
from creating or unlinking any file type outside a fixed list, so Android
itself could no longer write Download. Linux payloads get create/read/write on
`media_rw_data_file`, never execute; the launch glue only passes the bind fd.
Scope comes from the sandbox: linuxd opens only the granted Download subtree,
and the payload's mount namespace contains nothing else of /data/media. Labels
do not encode per-launch consent.
This policy was reviewed as text, not compiled or verified in enforcing mode.

After a granted session exits, linuxd runs fixed native `cmd activity broadcast
--async --user <u> -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d
file:///storage/emulated/<u>/Download`. The exported MediaProvider receiver
queues MediaServiceV2, which calls MediaService.onScanFile; ModernMediaScanner
accepts a directory and walks it recursively. This is a supported stock receiver
path despite the action's deprecated public-API status. It survives stub death
and requires no compositor or bridge API change. Completion/indexing latency
and cross-user delivery still require device verification.

Source references and exact validation results are recorded in
`out/pc-logs/agents/downloads-grant-result.md` (machine-local report).

## Per-user storage (2026-10-11)

The private Downloads destination is inside
`/data/misc_ce/<uid/100000>/matonos-linux/home/<uid>/Downloads`.
The user-dirs config follows the same CE HOME. Both granted and denied
launches wait for CE availability; linuxd returns an unlock error before
creating private data when the user is locked. Raw Android Downloads sources
remain `/data/media/<uid/100000>/Download` and the grant protocol is unchanged.
Install/staging/runtime inputs now use the per-user DE layout described in
`install/linuxd/STORAGE-LAYOUT.md`; the home bind remains first in bwrap argv.
The helper staging script lists linux-paths.h and the Downloads headers;
external helper copies and prebuilt binaries still require coordinator staging.

## Generated stub runtime sockets (2026-10-11)

Stubs also use stock `untrusted_app_*`, with certificate authentication in
linuxd/the System Bridge. The same create/unlink whitelist rules out a custom
runtime socket label. Their UID-owned runtime tree now uses stock
`media_rw_data_file`, including sockets; see
[Linux stub SELinux](LINUX-STUB-SELINUX.md) for the DAC/MLS boundary and pending
enforcing checks. No Android app receives access to the Linux-zone home type.
