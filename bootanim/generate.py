#!/usr/bin/env python3
"""Render MatonOS's stock-style wordmark boot animation.

Requires Pillow. All inputs are local; generation does not use the network.
The stored ZIP layout is the format consumed directly by BootAnimation.
"""

from __future__ import annotations

import argparse
import math
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent
FONT = ROOT / "fonts" / "Montserrat-SemiBold.ttf"
DEFAULT_OUT = ROOT / "out"

# The AOSP fallback uses a 512x128 logo, a 2048-pixel shine texture, and a
# 12fps render loop. Its shine traverses the texture at about 240 px/second.
LOGO_SIZE = (512, 128)
SHINE_WIDTH = 2048
SHINE_SPEED = 240.0
DEFAULT_FPS = 12
DEFAULT_SIZE = (1280, 720)


def make_wordmark() -> Image.Image:
    """Return an antialiased white-on-transparent MatonOS mask, 512x128."""
    scale = 4
    canvas = Image.new("L", (LOGO_SIZE[0] * scale, LOGO_SIZE[1] * scale), 0)
    draw = ImageDraw.Draw(canvas)

    # Fit the exact wordmark into the stock logo's wide, shallow proportions.
    for size in range(512, 20, -1):
        font = ImageFont.truetype(str(FONT), size * scale)
        bounds = draw.textbbox((0, 0), "MatonOS", font=font, stroke_width=0)
        if bounds[2] - bounds[0] <= (LOGO_SIZE[0] - 20) * scale and bounds[3] - bounds[1] <= 100 * scale:
            break

    width = bounds[2] - bounds[0]
    height = bounds[3] - bounds[1]
    x = (canvas.width - width) // 2 - bounds[0]
    y = (canvas.height - height) // 2 - bounds[1]
    draw.text((x, y), "MatonOS", fill=255, font=font)
    return canvas.resize(LOGO_SIZE, Image.Resampling.LANCZOS)


def make_shine() -> Image.Image:
    """Build a periodic soft gray shine band similar to AOSP's stock texture."""
    shine = Image.new("RGB", (SHINE_WIDTH, LOGO_SIZE[1]))
    pixels = shine.load()
    center = SHINE_WIDTH / 2
    for x in range(SHINE_WIDTH):
        distance = abs(x - center)
        # A broad super-Gaussian trough: bright at both ends and softly shaded
        # through the middle, matching the stock shine's low-contrast gray.
        shade = round(255 - 153 * math.exp(-((distance / 322.0) ** 4)))
        for y in range(LOGO_SIZE[1]):
            pixels[x, y] = (shade, shade, shade)
    return shine


def crop_wrapped(image: Image.Image, x: int, width: int) -> Image.Image:
    x %= image.width
    if x + width <= image.width:
        return image.crop((x, 0, x + width, image.height))
    first = image.crop((x, 0, image.width, image.height))
    second = image.crop((0, 0, width - first.width, image.height))
    result = Image.new(image.mode, (width, image.height))
    result.paste(first, (0, 0))
    result.paste(second, (first.width, 0))
    return result


def render_frames(size: tuple[int, int], fps: int) -> list[Image.Image]:
    # One stock-speed traversal, with the frame count chosen so the loop closes
    # exactly at its first shine phase (8.53 seconds at 12fps).
    frame_count = round(SHINE_WIDTH / SHINE_SPEED * fps)
    displacement = SHINE_WIDTH / frame_count
    mask = make_wordmark()
    shine = make_shine()
    x = (size[0] - LOGO_SIZE[0]) // 2
    y = (size[1] - LOGO_SIZE[1]) // 2
    frames: list[Image.Image] = []

    for index in range(frame_count):
        # The source texture moves right behind the fixed wordmark.
        source_x = round(SHINE_WIDTH - index * displacement)
        logo = crop_wrapped(shine, source_x, LOGO_SIZE[0])
        logo.putalpha(mask)
        frame = Image.new("RGB", size, (0, 0, 0))
        frame.paste(logo, (x, y), logo.getchannel("A"))
        frames.append(frame)
    return frames


def write_animation(out: Path, frames: list[Image.Image], fps: int) -> None:
    part = out / "part0"
    part.mkdir(parents=True, exist_ok=True)
    for index, frame in enumerate(frames):
        frame.save(part / f"{index:04d}.png", optimize=True)

    # A count of zero loops part0 until BootAnimation observes boot complete.
    size = frames[0].size
    desc = f"{size[0]} {size[1]} {fps}\np 0 0 part0\n"
    (out / "desc.txt").write_text(desc, encoding="ascii")

    archive = out / "bootanimation.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as bundle:
        bundle.write(out / "desc.txt", "desc.txt")
        for path in sorted(part.glob("*.png")):
            bundle.write(path, f"part0/{path.name}")


def write_preview(out: Path, frames: list[Image.Image], fps: int) -> None:
    preview_dir = out / "preview"
    preview_dir.mkdir(parents=True, exist_ok=True)
    indices = sorted({0, len(frames) // 4, len(frames) // 2, (3 * len(frames)) // 4, len(frames) - 1})
    for index in indices:
        frames[index].save(preview_dir / f"frame-{index:04d}.png", optimize=True)

    # GIF stores durations in 10ms ticks. Distribute 80/90ms steps at 12fps
    # so the complete preview keeps the same total duration as the PNG loop.
    gif_durations = [
        (round((index + 1) * 100 / fps) - round(index * 100 / fps)) * 10
        for index in range(len(frames))
    ]
    frames[0].save(
        preview_dir / "matonos-bootanimation.gif",
        save_all=True,
        append_images=frames[1:],
        duration=gif_durations,
        loop=0,
        optimize=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory (default: bootanim/out)")
    parser.add_argument("--width", type=int, default=DEFAULT_SIZE[0])
    parser.add_argument("--height", type=int, default=DEFAULT_SIZE[1])
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS)
    args = parser.parse_args()
    if args.width < LOGO_SIZE[0] or args.height < LOGO_SIZE[1] or args.fps < 1:
        parser.error("resolution must fit the stock-size logo and fps must be positive")

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    frames = render_frames((args.width, args.height), args.fps)
    write_animation(out, frames, args.fps)
    write_preview(out, frames, args.fps)
    print(f"Wrote {len(frames)} frames at {args.width}x{args.height} {args.fps}fps to {out}")


if __name__ == "__main__":
    main()
