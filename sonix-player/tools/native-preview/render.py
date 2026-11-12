#!/usr/bin/env python3
"""Render the device-size screenshots as RGB PNGs."""
from array import array
from itertools import product
from pathlib import Path
import argparse
import os
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parent
APP = ROOT.parents[1]
BUILD = ROOT / "build"
OUTPUT = ROOT / "screenshots"
WIDTH = 480

def chunk(name, content):
    body = name + content
    return struct.pack(">I", len(content)) + body + struct.pack(">I", zlib.crc32(body))

def capture(page, height, theme, text, language, playing=False, input_test=False):
    name = f"{page}-{height}-{theme}-{text}"
    if language != "English":
        name += "-" + language
    if playing:
        name += "-playing"
    raw = BUILD / (name + ".rgb565")
    env = os.environ.copy()
    env.pop("SONIX_PREVIEW_PLAYING", None)
    env.pop("SONIX_PREVIEW_INPUT_TEST", None)
    if playing:
        env["SONIX_PREVIEW_PLAYING"] = "1"
    if input_test:
        env["SONIX_PREVIEW_INPUT_TEST"] = "1"
    subprocess.run(
        [str(BUILD / "sonix_preview"), page, str(height), theme, text, language, str(raw)],
        cwd=APP,
        env=env,
        check=True,
        timeout=20,
    )
    pixels = array("H", raw.read_bytes())
    if len(pixels) != WIDTH * height or len(set(pixels)) < 32:
        raise RuntimeError(f"{name} did not produce a complete, nonblank screenshot")
    data = bytearray()
    for row in range(height):
        data.append(0)
        for pixel in pixels[row * WIDTH:(row + 1) * WIDTH]:
            red = pixel >> 11
            green = (pixel >> 5) & 0x3F
            blue = pixel & 0x1F
            data.extend(((red << 3) | (red >> 2), (green << 2) | (green >> 4), (blue << 3) | (blue >> 2)))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(data))
    png += chunk(b"IEND", b"")
    output = OUTPUT / (name + ".png")
    output.write_bytes(png)
    print(output, flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--page", nargs="+", default=["home", "apps", "music", "appearance"])
    parser.add_argument("--height", nargs="+", type=int, default=[800, 720])
    parser.add_argument("--theme", nargs="+", default=["dark", "light"])
    parser.add_argument("--text", nargs="+", default=["normal", "large"])
    parser.add_argument("--language", nargs="+", default=["English"])
    parser.add_argument("--playing", action="store_true", help="Use the deterministic playing-track fixture")
    parser.add_argument("--input-test", action="store_true", help="Exercise home/drawer taps before capture")
    args = parser.parse_args()
    OUTPUT.mkdir(exist_ok=True)
    for case in product(args.page, args.height, args.theme, args.text, args.language):
        capture(*case, playing=args.playing, input_test=args.input_test)
