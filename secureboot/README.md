# MatonOS Secure Boot (development implementation)

## Design

The opt-in Secure Boot chain is:

```text
UEFI (Microsoft third-party UEFI CA in db)
  -> vendor-signed shimx64.efi (EFI/BOOT/BOOTX64.EFI)
  -> MokManager (mmx64.efi) enrolls MatonOS MOK certificate once
  -> MatonOS-signed systemd-boot (shim's fixed grubx64.efi name)
  -> MatonOS-signed UKI (systemd-stub + kernel + vendor/generic initrds)
  -> Android init
```

`tools/make-live.sh -S` and `tools/make-payload.sh -S` opt into this path;
without `-S`, image creation retains its existing unsigned layout. Shim is
copied byte-for-byte from a locally installed distro package or a caller
selected `MATON_SHIM_EFI`; MokManager comes from the corresponding package
(`MATON_MOK_MANAGER_EFI`). No shim binary is checked into this repository.
The shim must be a currently accepted x86_64 binary for the target firmware:
firmware DB/DBX and shim SBAT policy differ across PCs, so no one binary can
be promised to boot every Secure Boot machine. The host's Ubuntu shim 15.8 is
Microsoft CA2011 signed; Fedora 16.1-7 is dual-signed by Microsoft CA2011 and
CA2023. The OVMF VARS used here has both Microsoft certificates, a `shim,4`
SBAT floor, and no meaningful DBX image revocations. Fedora's generation is
above that floor, and the controlled trace confirms firmware accepts the
shim. The 0x1A screen was returned after shim rejected its MatonOS second
stage, rather than by the firmware rejecting shim.

The helper rejects
2011-only shims by default;
`MATON_ALLOW_2011_ONLY_SHIM=1` exists only for legacy experiments.
Fedora's `shim-x64-16.1-7` package provides an x86_64 shim dual-signed by the
2011 and 2023 Microsoft UEFI CAs; use a current dual-signed vendor package for
broader old/new firmware coverage, after testing against each target. This
Fedora RPM identifies its package license as BSD-3-Clause. Microsoft
transitioned new UEFI signing to the 2023 CA on
2026-06-26; expiration of the 2011 certificate alone does not instantly revoke
already signed shims, but old-only shims won't work where the 2011 CA is absent
or revoked. SBAT/DBX revocations can also reject a previously accepted shim.
The current upstream SBAT policy file (last listed revocation dated
2025-11-24) sets a `shim,4` threshold. The Ubuntu 15.8 shim reports generation
4; the Fedora 16.1 binary reports `shim,16.1-1`. Both exceed that published
threshold, subject to newer local firmware DBX/hash revocations. The helper
allows explicit shim/MokManager paths for package-specific choices. Check the
exact artifact against the machine's current revocation state before release.
Ubuntu's shim-signed copyright file identifies its upstream shim binary as
BSD-2-Clause; Fedora's `shim-x64` package page lists BSD-3-Clause. The image
builders and installer preserve the Fedora package license text and source
provenance in `EFI/BOOT/licenses/Fedora-shim-x64-BSD-3-Clause.txt`; copied
vendor binaries remain unchanged. Recheck the precise vendor package terms
when changing shim versions.

The stage was isolated with Debian Trixie shim-signed 16.1-2 and its matching
MokManager and signed GRUB. Debian shim verifies and starts Debian GRUB at
`EFI/BOOT/grubx64.efi`; this proves the shim and `EFI/BOOT/mmx64.efi` names
and Debian SBAT generations are accepted by this OVMF. Replacing only GRUB
with MatonOS-signed systemd-boot reproduces the violation. The verbose shim
trace reports `Binary is not authorized` / `Security Policy Violation` for
`\\EFI\\BOOT\\grubx64.efi`, then successfully verifies
`\\EFI\\BOOT\\mmx64.efi` and its SBAT data. Thus 0x1A is the rejected
MatonOS second stage before MOK enrollment, not a rejected shim or manager.
The signed Debian GRUB control image reaches its menu. Diagnostic screenshots,
images, and serial traces are in `/mnt/data/aosp/out/pc-logs/secureboot/`.

The shim's x86_64 second-stage name is fixed: systemd-boot is installed as
`EFI/BOOT/grubx64.efi`. The systemd-boot binary includes upstream `.sbat`
metadata (`systemd-boot,1`); each MatonOS UKI gets a `matonos,1` SBAT row as
well as the standard SBAT header. Bump MatonOS's generation when shipping a
security-replacement UKI and keep it monotonic; old images can then be revoked
by a later shim SBAT policy. Don't copy an SBAT section from an unrelated
vendor binary or edit the signed shim.

For the local OVMF test, `/usr/share/OVMF/OVMF_VARS_4M.ms.fd` was checked
against Microsoft's UEFI CA 2023 certificate and already contains that
certificate in `db`; a derived test template is saved outside the repo at
`/mnt/data/aosp/out/pc-logs/secureboot/OVMF_VARS_4M.ms-2023.fd`.

### Keys

`secureboot/secureboot.sh` creates one explicitly DEVELOPMENT-only RSA
3072 key hierarchy on first use. By default all signing files live in
`$HOME/.config/matonos/secureboot-dev` (override with
`MATON_SECUREBOOT_KEY_DIR`). The private key and combined kernel signing key
are mode 0600, in a mode-0700 directory, outside git and are never copied to
an image. The public certificate is emitted as PEM, DER, and the Linux kernel
combined key file. The DER certificate is public and is placed on the ESP for
manual MOK enrollment. Back up the directory offline; losing it means old
signed boot assets/modules cannot be updated under the same MOK. These dev
keys are not release keys: production needs offline/HSM-backed custody,
rotation/recovery policy, independent release signing, and a documented MOK
reset/re-enrollment path.

Use exactly this MatonOS key for EFI boot assets and optional module signing.
`tools/build-kernel.sh -S` sets `CONFIG_MODULE_SIG_KEY` to the combined key,
enables `CONFIG_MODULE_SIG_FORCE`, and signs all built modules; it leaves the
default kernel config untouched without `-S`. This needs a full kernel rebuild
and every shipped/add-on module must be signed with this key. The build flag is
opt-in to avoid changing other agents' current kernel/image build. The kernel
image itself is signed as part of each UKI. The raw staged `prebuilt/bzImage`
remains unsigned; do not place it in a Secure Boot ESP. QEMU enforcement of
unsigned modules depends on booting a kernel built with `-S`.

The add-ons build uses the same `$MATON_SECUREBOOT_KEY_DIR/kernel-signing-key.pem`
for modules. Kernel `vermagic` remains release-specific; a correctly signed
module for a different kernel still must not be installed.
The add-on repository build pipeline must sign every precompiled module for
each kernel release with this same key. The installer/service validates
signatures and rejects unsigned or wrong-key modules once
`CONFIG_MODULE_SIG_FORCE` is enabled; before that enforcement build ships, it
may report the signature mismatch as a warning for development diagnostics.

### CPU microcode early load

Every live and installed x86_64 boot profile prepends an uncompressed early
microcode cpio archive before Android's vendor and generic ramdisks. Secure
Boot UKIs embed this archive as their first initrd section, so the microcode
data is covered by the MatonOS UKI signature. Non-Secure-Boot loader entries
refer to the same archive as their first `initrd`. The archive contains the
kernel's standard `kernel/x86/microcode/AuthenticAMD.bin` and
`GenuineIntel.bin` paths. This provides updates where the CPU supports OS
loading; BIOS microcode remains preferred and some processors only accept BIOS
updates.

`secureboot/microcode.sh` fetches immutable source commits into the local
microcode cache and assembles the early cpio. The current pins are
linux-firmware `9b858e5bb58d7bf1fc4d8818cb9100aed6d46f6a` for AMD and Intel
`microcode-20260812` commit `927e65c8d5a6e4ec05cc74b1778283ab2284d0c1`.
Intel's repository marks its microcode package for binary redistribution with
the included license; AMD's per-file licensing notices are in linux-firmware
`WHENCE`. The Intel license and AMD `WHENCE` notices travel in the installer
payload and on each ESP under `EFI/BOOT/licenses/`. Update the pinned commits
deliberately when refreshing CPU fixes; rebuild and re-sign every UKI so the
early archive and kernel stay a single signed boot object.

### MOK enrollment

The first Secure Boot boot is an interactive firmware ceremony. On shim's MOK
management prompt, choose **Enroll key from disk**, browse to
`EFI/BOOT/matonos-dev.der`, confirm the displayed MatonOS Development Secure
Boot Key, and reboot. Shim then accepts the MatonOS-signed systemd-boot and UKI.
If the prompt is missed, reboot and enter shim's MOK Manager using its prompt
(key varies by shim/firmware). Don't turn Secure Boot off as a normal
workaround; first enrollment requires physical-console confirmation by design.
`mokutil --sb-state` inside MatonOS should report SecureBoot enabled after
boot. The key is enrolled into shim's MOK list, not firmware PK/KEK/db.

### ESP layout

Live USB, `make-live.sh -S`:

```text
EFI/BOOT/BOOTX64.EFI                 vendor shim (unmodified)
EFI/BOOT/mmx64.efi                   matching MokManager
EFI/BOOT/grubx64.efi                 MatonOS-signed systemd-boot
EFI/BOOT/matonos-dev.der             public key for MokManager
EFI/systemd/systemd-bootx64.efi     same signed systemd-boot
EFI/Linux/matonos-live.efi           signed UKI: bzImage + microcode cpio + vendor ramdisk + ramdisk + Ventoy pre-init
EFI/Linux/matonos-live-debug.efi     same, debug kernel command line
EFI/BOOT/licenses/                   Intel license and AMD WHENCE source notices
loader/entries/*.conf                 entries select the UKI; command line is signed inside it
```

Installed disks use the same shim/MokManager/systemd-boot locations. Each A/B
slot is a signed UKI in the ESP (for example `EFI/Linux/matonos-a.efi` and
`matonos-b.efi`) containing kernel, microcode, ramdisks, and the slot-specific
command line including `androidboot.slot_suffix`. systemd-boot BOOT COUNTING
uses `+N` entry names for automatic rollback; the boot-control HAL marks a
successful boot by renaming the entry and selects the default through
`loader.conf`/entry names. Firmware uses fallback `EFI/BOOT/BOOTX64.EFI`
(shim → systemd-boot), with no EFI NVRAM writes. Keep `editor no` and treat
the ESP as security-sensitive. Live UKIs embed their complete command line,
including the ESP PARTUUID.

## Re-signing and updates

Every kernel update rebuilds and signs its UKI with this MOK; both live media
and each inactive A/B installed slot's UKI must be updated before switching
slots. The updater must copy a newly signed UKI into the inactive slot path
and only select it after signature verification. A
systemd-boot or shim update also needs a trusted vendor-signed shim and a
Maton-signed systemd-boot second stage, plus the matching MokManager. All new
MatonOS EFI binaries need valid SBAT metadata. Module updates must use the same
key for `CONFIG_MODULE_SIG_FORCE`; signing an updated boot UKI alone does not
make old/unsigned modules load.

If the key is lost, the installed MOK cannot authorize replacement MatonOS
boot assets or modules. Recovery must be physical: enroll a newly generated
key through MokManager, then install a matching signed bootloader/UKI/module
set. Do not ship the private key in the payload or image.

## Build and verification

Prerequisites on the build host: `sbsigntool`, `systemd-ukify`, OpenSSL, git,
cpio, a vendor shim + matching MokManager package, and mtools for live-image
assembly. The first image build fetches the pinned CPU microcode repositories.
The shim helper defaults to Ubuntu's package path but requires a 2023 CA
signature; point it to a current dual-signed package. On Fedora the paths are
versioned under `/usr/lib/efi/shim/<version>/EFI/fedora/`.

```sh
export MATON_SHIM_EFI=/usr/lib/efi/shim/16.1-7/EFI/fedora/shimx64.efi
export MATON_MOK_MANAGER_EFI=/usr/lib/efi/shim/16.1-7/EFI/fedora/mmx64.efi
```

Then build:

```sh
tools/make-live.sh -S -o /mnt/data/aosp/out/target/product/pc_x86_64
tools/make-payload.sh -S -o /mnt/data/aosp/out/target/product/pc_x86_64 \
  -k prebuilt/bzImage -d /tmp/matonos-secure-payload
# For a kernel enforcing signed modules (full kernel build, coordinator only):
tools/build-kernel.sh -S
```

Before distributing an image, inspect the shim signature and SBAT on the
exact firmware generation being targeted, and verify all MatonOS PE images:

```sh
sbverify --list /path/to/EFI/BOOT/BOOTX64.EFI
sbverify --cert "$HOME/.config/matonos/secureboot-dev/matonos-dev.pem" /path/to/grubx64.efi
sbverify --cert "$HOME/.config/matonos/secureboot-dev/matonos-dev.pem" /path/to/matonos-live.efi
objdump -s -j .sbat /path/to/matonos-live.efi
```

QEMU verification used `/mnt/data/aosp/out/pc-logs/secureboot/debian-fresh-live.img`,
assembled with Debian Trixie shim and matching manager, MatonOS-signed
systemd-boot, and signed UKIs. OVMF Secure Boot ran with Microsoft keys, 4 GiB
RAM, and ports 5569–5578 (never 5555).

The shim trace `serial-debian-systemd.log` identifies the rejected image:
shim accepts itself, then rejects our systemd-boot at `EFI/BOOT/grubx64.efi`
as `Binary is not authorized`, before loading `EFI/BOOT/mmx64.efi` (MokManager).
These are the expected shim paths. A Debian GRUB control image reached its
menu, proving shim and manager verification succeeded. Microsoft-key OVMF vars
had SecureBoot ON, Microsoft UEFI CAs 2011/2023, an empty-file-only dbx, and
SBAT floors `shim,4`, `grub,4`, `grub.peimage,2`. Fedora 16.1-7's `shim,16.1`
is above the shim floor and its Microsoft signatures are accepted; the
verified run used Debian Trixie 16.1-2 (`shim,4`). `.sbat` is present in shim
and manager (`shim,4`), systemd-boot (`systemd-boot,1`), and the UKI
(`systemd-stub,1`, `matonos,1`).

MokManager enrollment was completed interactively: **Enroll key from disk**,
volume `MATONOS`, `EFI/BOOT/matonos-dev.der`, then **Continue** and **Yes**.
The disposable vars file contains a 1170-byte `MokList` with Secure Boot ON.
The fresh image then booted: `sys.boot_completed=1`, kernel log reported
`Secure boot enabled` and loaded `MatonOS Development Secure Boot Key`, and
`debian-final-home.png` captures Android's home screen. `mokutil` is absent;
the OVMF variable dump and kernel log provide state evidence.

For the negative test, only the Authenticode signature was removed from
`EFI/Linux/matonos-live.efi`. systemd-boot displayed `Verification failed:
Security Policy Violation` and `Error loading \\EFI\\Linux\\matonos-live.efi:
Security violation`; Android did not boot. This verifies rejection of the
unsigned UKI/kernel bundle. The kernel reports `CONFIG_MODULE_SIG=y` but not
`CONFIG_MODULE_SIG_FORCE`; unsigned module refusal still needs the optional
`tools/build-kernel.sh -S` kernel build and a coordinator image build. No
kernel was rebuilt for these tests.

## Real hardware test checklist

1. On a disposable USB, build with `make-live.sh -S`, enable firmware Secure
   Boot while retaining Microsoft third-party UEFI CA trust, and select the
   USB's UEFI entry.
2. Enroll the MatonOS Development Secure Boot Key at MokManager's physical
   prompt, reboot, and verify MatonOS reaches its home screen.
3. In a root shell, confirm `mokutil --sb-state`, `/sys/kernel/security/lockdown`,
   and `dmesg` Secure Boot/lockdown state. Verify `sbverify` recognizes the
   signed UKI before writing the USB.
4. Repeat on each target firmware family (especially systems with updated
   2023-only trust stores, current DBX/SBAT revocation data, and Microsoft
   third-party CA disabled). An unsupported shim must fail closed; retain a
   normal non-Secure-Boot image for those devices.
5. Build/install a Secure Boot payload on a spare disk; check the installed
   ESP has the same shim chain and that the boot entry passes the disk's
   correct boot-device path.
6. With a `-S` kernel build, try loading an unsigned test module and confirm
   rejection; then sign a module with the MatonOS key and confirm it loads.

## Current gaps

- QEMU verified signed UKI acceptance and unsigned UKI/kernel-bundle rejection.
  It did not verify unsigned module rejection because this image lacks
  `CONFIG_MODULE_SIG_FORCE`; that needs a `-S` kernel build and coordinator
  rebuild. `mokutil` is also absent from the image.
- QEMU has not yet verified early microcode application. Check
  `dmesg | grep -i microcode` and `/proc/cpuinfo` on both AMD and Intel
  hardware; the local QEMU host is AMD, so it cannot validate Intel loading.
- OVMF runtime variables from the Microsoft-key `OVMF_VARS_4M.ms.fd` template
  show `SecureBootEnable=ON`; `db` contains Microsoft UEFI CAs 2011 and 2023;
  `dbx` has only the SHA-256 of an empty file; and the active SBAT level is
  `sbat,1,2024040900 / shim,4 / grub,4 / grub.peimage,2`. Fedora
  `shim-x64-16.1-7` has Microsoft CA2011 and CA2023 signatures and advertises
  SBAT `shim,16.1`, above the `shim,4` floor. Its first-stage signature and
  SBAT are accepted; the Debian shim verbose trace identifies MatonOS
  `grubx64.efi` as the rejected image. Both tested shim builds use the expected
  `EFI/BOOT/mmx64.efi` and `EFI/BOOT/grubx64.efi` paths.
- Debian Trixie `shim-signed` 16.1-2 has Microsoft CA2011 signing and SBAT
  `shim,4` + `shim.debian,1`; its matching signed helper is `mmx64.efi.signed`
  (Debian Secure Boot CA, SBAT `shim,4` + `shim.debian,1`). Its signed GRUB is
  SBAT `grub,5`, `grub.debian,5`, and `grub.peimage,2`, meeting the OVMF levels.
  The package payload SHA-256 values match Debian's published hashes. Debian's
  copyright files state shim is BSD-2-Clause and helper signing materials are
  public domain; carry the relevant copyright notices when redistributing.
- Fresh QEMU evidence is in `/mnt/data/aosp/out/pc-logs/secureboot/`:
  MokManager screenshots `debian-mok-ui-attempt.png`,
  `debian-mok-file-browser.png`, `debian-mok-boot-dir.png`,
  `debian-mok-final-confirm.png`, and `debian-mok-enrolled-screen.png`;
  `debian-final-home.png` captures the enrolled Android boot; and
  `serial-debian-unsigned-current.log` plus
  `debian-unsigned-current-8s.png` show unsigned UKI rejection. The disposable
  enrolled variables are in `vars-debian-interactive-enrolled.fd`.
- Installed UKI command line parameters remain mutable as described above.
- systemd-boot's SBAT record is upstream's generation 1, not a MatonOS-forked
  record; MatonOS's own generation currently lives in the signed UKI.
- No kernel config symbols outside `tools/build-kernel.sh -S` are changed;
  `CONFIG_MODULE_SIG_FORCE` is opt-in pending successful rebuild and tests.
