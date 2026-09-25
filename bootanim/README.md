# MatonOS boot animation

This area generates a stock-style `MatonOS` wordmark animation. It uses the
Montserrat SemiBold font under the SIL Open Font License (license is kept in
`fonts/OFL.txt`). The mask keeps the wordmark fixed and lets a soft gray shine
move from left to right through it, on a black background.

## Design

The local fallback in `frameworks/base/cmds/bootanimation/BootAnimation.cpp`
uses a 512x128 logo, a 2048-pixel shine texture, and a 12 fps render loop. The
generator uses those logo proportions and timing: the shine moves at roughly
240 pixels per second and repeats about every 8.5 seconds. The animation
contains one infinitely looping part, so BootAnimation keeps playing it until
boot completes. Frames are 1280x720 and the wordmark is centered at the stock
512x128 size.

All inputs are local, so generation has no build-time network dependency.
Pillow is the only Python dependency.

## Generate and preview

From the device tree root:

```sh
python3 bootanim/generate.py
```

This writes the uncompressed `bootanimation.zip`, frame PNGs, and preview
assets to `bootanim/out/`. The preview GIF is
`bootanim/out/preview/matonos-bootanimation.gif`; five representative PNGs are
also in that directory. `out/` is ignored by Git.

The one-time product rule is prepared in `bootanim/bootanim.mk` and is not
included from `device.mk`; the coordinator can wire it in during the next
planned product-graph change.

## Test on real hardware

After the coordinator wires in the product rule and builds a fresh image:

1. Install and boot the image on a UEFI PC.
2. Confirm the centered MatonOS wordmark appears on black and the subtle shine
   travels left to right, repeating smoothly while services start.
3. Confirm the animation exits to the launcher after boot completes.
4. Repeat on each test PC (Ryzen 5800X/RX 6600, Surface Pro 3, and HP ProDesk
   600 G1) to check that the image works independently of graphics hardware.

## Open issues and verification

- No AOSP patch, new daemon, SELinux policy, or kernel change is needed.
- The product rule is intentionally not wired yet, so the animation has not
  been tested in a booted image. A fresh image/QEMU run is deferred to the
  coordinator's next build.
- No shared-file changes are requested.
