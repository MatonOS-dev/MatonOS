#!/usr/bin/env bash
# check-selinux-labels.sh: find vendor programs that init would refuse to run.
#
# init won't start a service or `exec` a program whose file has no SELinux
# domain transition (e.g. labelled plain vendor_file), even in permissive
# mode ("has incorrect label or no domain transition"). This checks every
# `service` and `exec` in the built vendor init scripts against the compiled
# file_contexts and fails if a binary would end up without an *_exec label.
#
# Usage: ./check-selinux-labels.sh [-o <product out dir>]
# Run after an AOSP build (build.sh does this automatically).
set -Eeuo pipefail

DEVICE_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")
AOSP=$(readlink -f "$DEVICE_DIR/../../..")
PRODUCT_OUT=$AOSP/out/target/product/pc_x86_64
while getopts "o:h" opt; do
  case $opt in
    o) PRODUCT_OUT=$OPTARG ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
  esac
done

[[ -d $PRODUCT_OUT/vendor/etc/init ]] || { echo "ERROR: no built vendor in $PRODUCT_OUT" >&2; exit 1; }

exec python3 - "$PRODUCT_OUT" <<'PYEOF'
import glob, os, re, sys

out = sys.argv[1]

# --- file_contexts: (regex, type); pick the most specific match like libselinux
rules = []
for fc in (f"{out}/system/etc/selinux/plat_file_contexts",
           f"{out}/vendor/etc/selinux/vendor_file_contexts"):
    if not os.path.exists(fc):
        continue
    for line in open(fc):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        path, ctx = parts[0], parts[-1]
        if ctx == "<<none>>" or len(parts) == 3 and parts[1] not in ("--", "-f"):
            continue  # only regular-file rules matter for executables
        try:
            rx = re.compile(r"^(" + path + r")$")
        except re.error:
            continue
        stem = re.match(r"^[^.^$?*+|\[\](){}\\]*", path).group(0)
        rules.append((rx, len(stem), ctx.split(":")[2]))

def label(path):
    best = None
    for idx, (rx, stem, typ) in enumerate(rules):
        if rx.match(path):
            key = (stem, idx)          # longer literal stem wins, then later rule
            if best is None or key > best[0]:
                best = (key, typ)
    return best[1] if best else "unlabeled"

# --- programs started by init from vendor rc files
progs = []
for rc in sorted(glob.glob(f"{out}/vendor/etc/init/**/*.rc", recursive=True)):
    for raw in open(rc, errors="replace"):
        line = raw.strip()
        m = re.match(r"^service\s+(\S+)\s+(\S+)", line)
        if m:
            progs.append((rc, f"service {m.group(1)}", m.group(2)))
            continue
        m = re.match(r"^(exec|exec_background|exec_start)\s+(.*)$", line)
        if m and m.group(1) != "exec_start":
            args = m.group(2).split()
            if "--" in args:
                before, after = args[:args.index("--")], args[args.index("--") + 1:]
                if before and before[0].startswith("u:r:"):
                    continue            # explicit seclabel: init doesn't need a transition
                if after:
                    progs.append((rc, m.group(1), after[0]))

bad = 0
for rc, what, path in progs:
    if not path.startswith(("/vendor/", "/odm/")):
        continue                        # system/APEX binaries carry their own labels
    typ = label(path)
    ok = typ.endswith("_exec")
    if not ok:
        bad += 1
    print(f"{'OK  ' if ok else 'FAIL'} {typ:40s} {path}  ({what}, {os.path.relpath(rc, out)})")

if bad:
    print(f"\n{bad} vendor program(s) would be refused by init: add a file_contexts "
          f"entry with an *_exec type (sepolicy/vendor/file_contexts) and a domain.",
          file=sys.stderr)
    sys.exit(1)
print(f"\nAll {len(progs)} init-started vendor programs have exec labels.")
PYEOF
