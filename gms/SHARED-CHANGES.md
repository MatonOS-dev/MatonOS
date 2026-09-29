# Shared changes requested from the coordinator

The coordinator completed all requested shared integration changes:

- `device.mk` includes `gms/gms.mk`.
- `tools/build.sh` runs `gms/fetch-gms.sh` before the AOSP build.
- `preflight/checks.py` allows only the two user-approved GMS patches, 0002 signature spoofing and 0003 pinned GMS update.

The 2026-09-27 post-build preflight passed with these changes.
