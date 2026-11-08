# Handover 2026-11-08 (night)

## Done today
- lsa-trim `6ad060e`: emulated hard links survive /data dev drift; Steam's emulated links deleted.
- lsa-trim `4458c53`: namespace fix restored (TEMPDIAGs still in, per user).
- lsa-trim `f97eb83` (pushed, branch xwayland-ahb): virtual sigaltstack reports the proxy altstack
  as SS_ONSTACK while sp is on it, EPERM on change. Fixed TheoTown hang (Go launcher spun in
  needm). TheoTown now renders at 60 FPS.
- Factorio install as file-op stress test: Fully Installed, no Steam content errors, no proxy
  link/file errors in logcat (capture: scratchpad factorio.log). Not yet integrity-verified.
- Steam popups work (open centred in Steam window).

## Next, in order
1. Geometry Dash: Steam window went fully black after launch (Wine running). Determine if it is
   the game window loading or the black-boxes backlog bug. Ask user what they saw.
2. Factorio: run Steam "Verify integrity of game files" to confirm the install; ask user whether
   Factorio ran OK (it ran 20:55-20:59).
3. Webbed exit freeze: Steam reaper waits on launch-client. Check whether f97eb83 (same signal
   path) fixes it. Log reviews conflict: Wine `monitor_get_dpi` assertion after XRandR output
   disconnected/no EDID vs forwarder resolver "not found in any glibc namespace". Next step:
   relaunch + exit with capture.
4. Strip TEMPDIAGs once the namespace work is confirmed.
5. Investigate erofs "readahead error" kernel lines (device 0:203, 20:27, before install).
6. proxy `threads` test: rc=4, clone3 (435) ENOSYS unhandled — pre-existing.

## Backlog
- Steam/X11 popups placed in window centre (X11 position hints ignored).
- Mouse drift test; device-tree inputd/kernel/inputflinger commit (only if asked);
  app/window feature list; Xwayland DPI; adb Shift bug; marker-cache.py; close-fd log spam.
- Hard-link orphan placeholder (EBADMSG): codex report suggests journaled index removal; device
  store not inspected (permission denied even as su 0).
