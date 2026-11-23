#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK/sys/sys/firmware/acpi/bgrt" "$WORK/sys/sys/class/drm/card0-HDMI-A-1" "$WORK/wordmark"
printf '1\n' > "$WORK/sys/sys/firmware/acpi/bgrt/status"
printf '3\n' > "$WORK/sys/sys/firmware/acpi/bgrt/xoffset"
printf '2\n' > "$WORK/sys/sys/firmware/acpi/bgrt/yoffset"
printf 'connected\n' > "$WORK/sys/sys/class/drm/card0-HDMI-A-1/status"
printf '320x240\n' > "$WORK/sys/sys/class/drm/card0-HDMI-A-1/modes"
python3 - "$WORK" <<'PY'
import struct,sys
from pathlib import Path
p=Path(sys.argv[1])
# 8x6 bottom-up 24-bit BMP with a colored checkerboard and DWORD row padding.
w,h=8,6
rows=[]
for y in range(h):
    row=bytearray()
    for x in range(w): row += bytes((0, 180 if (x+y)%2 else 30, 240))
    row += b'\0' * ((4-(len(row)%4))%4); rows.append(row)
pixels=b''.join(reversed(rows)); off=54
(p/'sys/sys/firmware/acpi/bgrt/image').write_bytes(b'BM'+struct.pack('<IHHI',off+len(pixels),0,0,off)+struct.pack('<IiiHHIIiiII',40,w,h,1,24,0,len(pixels),2835,2835,0,0)+pixels)
# The wordmark asset format is magic, LE width/height, then RGBA pixels.
ww,wh=24,6; data=bytearray()
for y in range(wh):
  for x in range(ww):
    a=255 if (x+y)%4 else 0; data += bytes((255,255,255,a))
(p/'wordmark/wordmark.rgba').write_bytes(b'MRGBA1\0'+b'\0'+struct.pack('<II',ww,wh)+data)
PY
cc -std=c11 -Wall -Wextra -Werror -O2 "$ROOT/native/matonos-efi-logo2/matonos-efi-logo2.c" -o "$WORK/matonos-efi-logo2"
"$WORK/matonos-efi-logo2" --root "$WORK/sys" --output "$WORK/bootanimation.zip" --wordmark "$WORK/wordmark/wordmark.rgba"
unzip -t "$WORK/bootanimation.zip"
python3 - "$WORK/bootanimation.zip" <<'PY'
import io,struct,sys,zipfile
from PIL import Image
z=zipfile.ZipFile(sys.argv[1]); print(z.read('desc.txt').decode().strip())
for n in sorted(x for x in z.namelist() if x.endswith('.png')):
 d=z.read(n); assert d[:8]==b'\x89PNG\r\n\x1a\n'; w,h=struct.unpack('>II',d[16:24]); im=Image.open(io.BytesIO(d)); im.load(); print(f'{n}: decoded {w}x{h}')
 assert (w,h)==(320,240) and im.size==(320,240)
PY

# Also exercise 32-bit top-down BMP rows.
python3 - "$WORK" <<'PY32'
import struct,sys
from pathlib import Path
p=Path(sys.argv[1]); w,h=8,6
pixels=b''.join(bytes((30,120,240,255)) for _ in range(w*h)); off=54
bmp=b'BM'+struct.pack('<IHHI',off+len(pixels),0,0,off)+struct.pack('<IiiHHIIiiII',40,w,-h,1,32,0,len(pixels),2835,2835,0,0)+pixels
(p/'sys/sys/firmware/acpi/bgrt/image').write_bytes(bmp)
PY32
"$WORK/matonos-efi-logo2" --root "$WORK/sys" --output "$WORK/bootanimation-32bit.zip" --wordmark "$WORK/wordmark/wordmark.rgba"
unzip -t "$WORK/bootanimation-32bit.zip"

# Missing BGRT is a normal no-output fallback, not an error.
mkdir -p "$WORK/no-bgrt"
printf 'static-fallback' > "$WORK/no-bgrt.zip"
"$WORK/matonos-efi-logo2" --root "$WORK/no-bgrt" --output "$WORK/no-bgrt.zip" --wordmark "$WORK/wordmark/wordmark.rgba"
test "$(cat "$WORK/no-bgrt.zip")" = static-fallback
# Malformed BGRT must report failure while preserving the staged fallback.
printf '1\n' > "$WORK/sys/sys/firmware/acpi/bgrt/status"
printf 'not-a-bmp' > "$WORK/sys/sys/firmware/acpi/bgrt/image"
if "$WORK/matonos-efi-logo2" --root "$WORK/sys" --output "$WORK/no-bgrt.zip" --wordmark "$WORK/wordmark/wordmark.rgba"; then
  echo 'expected malformed BMP to fail' >&2; exit 1
fi
test "$(cat "$WORK/no-bgrt.zip")" = static-fallback
test ! -e "$WORK/no-bgrt.zip.tmp"
