# Coordinator integration proposal (not applied)

This prototype stays isolated. Merge only after review and once an image can
boot. The executable and rc are intended for the ODM bundle. The debug boot
loader entry must include `androidboot.matonos.debug=1`; init exposes it as
`ro.boot.matonos.debug=1`. Keep that value off every normal entry. Do not
change `make-live.sh` as part of this prototype.

## `bundle/contents.list`

Add these exact rows:

```text
file bootwatch/out/bin/matonos-bootwatch bin/matonos-bootwatch
file bootwatch/bootwatch.rc etc/init/matonos-bootwatch.rc
```

Build with `bootwatch/build.sh`; output is `bootwatch/out/bin/matonos-bootwatch`.

## Fixed policy files

Append to `sepolicy/matonos/matonos_driver.te`:

```te
# bootwatch: emit bounded diagnostics through logd.
allow matonos_driver logd:unix_dgram_socket sendto;
get_prop(matonos_driver, vendor_maton_prop)
```

`property_contexts` already covers `vendor.maton.*` with
`vendor_maton_prop`; no additional property labeling line is proposed. This
prototype currently reports through logcat and does not write report files
or set properties, so the property read grant is only needed if the
coordinator adds those planned features. Review the logd permission against
the actual compiled policy before merging.

## Init and loader

Use `bootwatch/bootwatch.rc` as `etc/init/matonos-bootwatch.rc`. It declares a
disabled, oneshot service and starts it only for
`ro.boot.matonos.debug=1`. Add `androidboot.matonos.debug=1` to the debug EFI
entry's kernel command line if it is missing. Do not add it to normal entries.

## Behavior and limits

No `contents.list`, policy, shared rc/VINTF, tools, or loader file has been
edited by this prototype. The current daemon polls logcat and
`sys.boot_completed`; it is deliberately read-only. Persistence, properties,
ANR/D-state/mount analysis, screen output, and report export remain future
milestones pending policy and runtime review.
