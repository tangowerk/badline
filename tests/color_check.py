#!/usr/bin/env python3
"""Colour path check (M-VK3b): GPU palette/picture settings and the C64 PAL pass against
references captured from the Denise CPU colour path before it was removed.

  tests/color_check.py --capture DIR --binary <old badline>   # store references (pre-VK3b build)
  tests/color_check.py --refs DIR [--binary target/release/badline] [--port 6467]

Each case starts a window instance (own XDG_CONFIG_HOME, fresh C64 config: nearest filter,
integer scaling), freezes a test picture (all 40x25 cells reversed spaces, colour
(col + 5 * row) & 15, border 14 / background 6) and takes a rendered screenshot, sampled at the
centre of every C64 pixel (window size independent).

  rgb-*  plain RGB path (old: CPU adjustPalette on Vulkan; new: GPU, PAL pass off):
         required within 1/255 per channel, every pixel
  pal-*  PAL/NTSC pass (Vulkan) against a Python port of Denise's c64_yuv preset (CPU luma/chroma
         table with 10 bit RGB, yuvEncoding + yuvDecoding passes; computed from the raw picture,
         the palette and the raster parity of the first line): required within 2/255. The GL
         presets could not serve as reference (they fail to compile here, also before VK3a).
         Side by side PNGs (reference | new) in the output folder.
"""

import argparse
import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time
import math
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcplib import Client, check, decode_png, image_of, text_of  # noqa: E402
import mcplib  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

PICTURE = {"video_saturation": 100, "video_contrast": 100, "video_brightness": 100, "video_gamma": 100}
SETTINGS = [
    ("default", {}),
    ("desat", {"video_saturation": 40}),
    ("vivid", {"video_saturation": 160, "video_contrast": 115, "video_brightness": 92, "video_gamma": 110}),
    ("flat", {"video_saturation": 90, "video_contrast": 80, "video_brightness": 118, "video_gamma": 85}),
    ("grey", {"video_saturation": 0, "video_gamma": 150}),
]

# (name, ntsc, config) -> compared old vs new
CASES = []
for pal in range(10):
    CASES.append((f"rgb-p{pal}-default", False, "rgb", {"palette": pal}))
for pal in (0, 2, 8):
    for name, s in SETTINGS[1:]:
        CASES.append((f"rgb-p{pal}-{name}", False, "rgb", dict({"palette": pal}, **s)))
CASES.append(("rgb-p4-ntsc", True, "rgb", {"palette": 4}))
CASES.append(("rgb-p0-vivid-ntsc", True, "rgb", dict({"palette": 0}, **SETTINGS[2][1])))
for pal in (0, 2):
    CASES.append((f"pal-p{pal}-default", False, "pal", {"palette": pal}))
    CASES.append((f"pal-p{pal}-vivid", False, "pal", dict({"palette": pal}, **SETTINGS[2][1])))
CASES.append(("pal-p4-ntsc", True, "pal", {"palette": 4}))


def config_lines(kind, settings, old):
    c = {"video_filter": 0, "integer_scaling": 1}
    c.update(PICTURE)
    c.update(settings)
    if old:
        c.update({"video_spectrum": 0, "video_crt_legacy": 0, "prepend_yuv_shader": 0})
        c["slang_loaded"] = ""
    else:
        c["video_pal_pass"] = 1 if kind == "pal" else 0
    return "".join(f"{k}:{v}\n" for k, v in c.items())


def denise_yuv(w, h, idx, colors, settings, ntsc, first_odd):
    """Denise's c64_yuv: -> RGB bytes"""
    sat = settings.get("video_saturation", 100) / 100
    con = settings.get("video_contrast", 100) / 100
    bri = settings.get("video_brightness", 100) - 100
    gamma = settings.get("video_gamma", 100) / 100
    rise, fall = 2.0, 1.2
    phase = math.radians(0.0 if ntsc else 22.5)
    enc = {}
    for even in (False, True):
        for i, (r, g, b) in enumerate(colors):
            # CPU: convertPaletteToLumaChroma + convertLumaChromaToRGB, 10 bit RGB
            if ntsc:
                y = 0.23485876230514607 * r + 0.6335007388077467 * g + 0.13164049888710716 * b
                u = 0.4409594767911895 * r - 0.27984362502847304 * g - 0.16111585176271648 * b
                v = 0.14630060102591497 * r - 0.5594814826856017 * g + 0.4131808816596867 * b
            else:
                y = 0.299 * r + 0.587 * g + 0.114 * b
                u = (b - y) * 0.493
                v = (r - y) * 0.877
            u, v, y = u * sat * con, v * sat * con, (y + bri) * con
            if ntsc:
                rgb = (y + 1.630 * u + 0.317 * v, y - 0.378 * u - 0.466 * v, y - 1.089 * u + 1.677 * v)
            else:
                rgb = (y + v / 0.877, y - 0.3939307027516405 * u - 0.5808092090310976 * v, y + u / 0.493)
            r2, g2, b2 = (min(max(int(c + 256 + 0.5), 0), 1023) - 256 for c in rgb)
            r2, g2, b2 = r2 / 255, g2 / 255, b2 / 255
            # yuvEncoding
            if ntsc:
                e = (0.23485876230514607 * r2 + 0.6335007388077467 * g2 + 0.13164049888710716 * b2,
                     0.4409594767911895 * r2 - 0.27984362502847304 * g2 - 0.16111585176271648 * b2,
                     0.14630060102591497 * r2 - 0.5594814826856017 * g2 + 0.4131808816596867 * b2)
            else:
                e = (0.299 * r2 + 0.587 * g2 + 0.114 * b2,
                     -0.147407 * r2 - 0.289391 * g2 + 0.436798 * b2,
                     0.614777 * r2 - 0.514799 * g2 - 0.099978 * b2)
            sn, cs = math.sin(phase), math.cos(phase)
            if not even:
                sn = -sn
            enc[even, i] = (e[0], e[1] * cs - e[2] * sn, e[2] * cs + e[1] * sn)

    def even_line(y):
        return ntsc or ((y + first_odd) & 1) == 0

    out = bytearray(w * h * 3)
    for y in range(h):
        ev = even_line(y)
        yb = 1 if y == 0 else y - 1
        evb = even_line(yb)
        row = idx[y * w:(y + 1) * w]
        rowb = idx[yb * w:(yb + 1) * w]
        for x in range(w):
            # yuvDecoding lumaLatency
            src = enc[ev, row[max(x - 3, 0)]][0]
            l, diff = src, 0.0
            for k in (2, 1, 0):
                t = enc[ev, row[max(x - k, 0)]][0]
                if src != t:
                    diff = t - l
                src = t
                if t > l:
                    l = min(l + diff / rise, t)
                elif t < l:
                    l = max(l + diff / fall, t)
            _, cu, cv = enc[ev, row[x]]
            if ntsc:
                rgb = (l + 1.630 * cu + 0.317 * cv, l - 0.378 * cu - 0.466 * cv, l - 1.089 * cu + 1.677 * cv)
            else:
                _, bu, bv = enc[evb, rowb[x]]
                cu, cv = (cu + bu) * 0.5, (cv + bv) * 0.5
                rgb = (l + 1.140251 * cv, l - 0.39393070 * cu - 0.58080921 * cv, l + 2.0283976 * cu)
            o = (y * w + x) * 3
            for j in range(3):
                out[o + j] = min(max(int(min(max(rgb[j], 0.0), 1.0) ** gamma * 255 + 0.5), 0), 255)
    return bytes(out)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def float_window(pid, width=960, height=720):
    """Hyprland: floating window of a fixed size (a tile next to other windows can get too small
    for an integer scaled picture). Silently skipped elsewhere."""
    try:
        clients = json.loads(subprocess.check_output(["hyprctl", "clients", "-j"]))
    except (OSError, subprocess.CalledProcessError):
        return
    addr = next((c["address"] for c in clients if c["pid"] == pid), None)
    if not addr:
        return
    win = f"window = 'address:{addr}'"
    for d in (f"hl.dsp.window.float({{ action = 'enable', {win} }})",
              f"hl.dsp.window.resize({{ x = {width}, y = {height}, {win} }})",
              f"hl.dsp.window.center({{ {win} }})"):
        subprocess.run(["hyprctl", "dispatch", d], capture_output=True)
    time.sleep(0.5)


def capture(args, name, ntsc, kind, settings, old, base):
    """-> (width, height, rgb bytes) sampled at C64 pixel centres, or None"""
    cfgdir = os.path.join(base, name)
    os.makedirs(cfgdir)
    cfg = os.path.join(cfgdir, "c64.ini")
    with open(cfg, "w") as f:
        f.write(config_lines(kind, settings, old))
    cmdline = [args.binary, "-config-c64", cfg, "-mcp", str(args.port)]
    if old:
        # reference builds (pre-VK3b, rgb cases only) still defaulted to OpenGL
        cmdline[3:3] = ["-renderer", "vulkan"]
    if ntsc:
        cmdline.insert(1, "-vic-6567R8")
    env = dict(os.environ, XDG_CONFIG_HOME=cfgdir)
    proc = subprocess.Popen(cmdline, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    client = Client(args.port)
    result = None
    try:
        for _ in range(100):
            try:
                client.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "color", "version": "1"}})
                break
            except OSError:
                time.sleep(0.1)
        float_window(proc.pid)
        client.call("reset", type="hard")
        r = client.call("wait_for_text", text="READY.", timeout=10, warp=True)
        if r.get("isError"):
            check(f"{name}: READY.", False, text_of(r))
            return None
        # freeze: IRQ vector past the keyboard scan / cursor blink, then the test picture
        client.call("memory_write", address="$0314", bytes=[0x81, 0xea])
        client.call("memory_write", address=0x0400, bytes=[0xa0] * 1000)
        colours = [(col + 5 * row) & 15 for row in range(25) for col in range(40)]
        client.call("memory_write", address=0xd800, bytes=colours, bank="cpu")
        client.call("memory_write", address=0xd020, bytes=[14, 6], bank="cpu")
        client.call("run_frames", frames=5)
        client.call("pause")
        client.call("run_frames", frames=2)
        raw = image_of(client.call("screenshot", scale=1))
        shot = image_of(client.call("screenshot", rendered=True))
        if raw is None or shot is None:
            check(f"{name}: screenshots", False)
            return None
        rw, rh, rc, rp = decode_png(raw)
        state = json.loads(text_of(client.call("picture")))
        sw, sh, sc, sp = decode_png(shot)
        with open(os.path.join(cfgdir, "rendered.png"), "wb") as f:
            f.write(shot)
        with open(os.path.join(cfgdir, "raw.png"), "wb") as f:
            f.write(raw)
        out = bytearray(rw * rh * 3)
        for y in range(rh):
            sy = int((y + 0.5) * sh / rh)
            for x in range(rw):
                sx = int((x + 0.5) * sw / rw)
                i = (sy * sw + sx) * sc
                out[(y * rw + x) * 3:(y * rw + x) * 3 + 3] = sp[i:i + 3]
        colors = [tuple(bytes.fromhex(c)) for c in state["colors"]]
        lookup = {c: i for i, c in reversed(list(enumerate(colors)))}
        idx = bytes(lookup.get(tuple(rp[i * rc:i * rc + 3]), 0) for i in range(rw * rh))
        result = (rw, rh, bytes(out), idx, colors, state)
        client.call("quit")
        proc.wait(10)
        errors = [l for l in proc.stderr.read().splitlines() if "validation error" in l]
        check(f"{name}: no Vulkan validation errors", not errors, "\n".join(errors[:3]))
        proc = None
    finally:
        if proc:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(HERE, "..", "target", "release", "badline"))
    ap.add_argument("--port", type=int, default=6467)
    ap.add_argument("--capture", help="store references from --binary (pre-VK3b build) in this folder")
    ap.add_argument("--refs", help="compare against the references in this folder")
    ap.add_argument("--filter", default="", help="only cases containing this text")
    args = ap.parse_args()
    if bool(args.capture) == bool(args.refs):
        ap.error("exactly one of --capture / --refs")

    base = tempfile.mkdtemp(prefix="badline-color-")
    for name, ntsc, kind, settings in CASES:
        if args.filter not in name:
            continue
        print(name)
        if args.capture:
            if kind != "rgb":
                continue
            img = capture(args, name, ntsc, kind, settings, True, base)
            if img:
                os.makedirs(args.capture, exist_ok=True)
                w, h, rgb = img[:3]
                with open(os.path.join(args.capture, name + ".json"), "w") as f:
                    json.dump({"width": w, "height": h}, f)
                with open(os.path.join(args.capture, name + ".rgb"), "wb") as f:
                    f.write(rgb)
                write_png(os.path.join(args.capture, name + ".png"), w, h, rgb)
                check(f"{name}: captured {w}x{h}", True)
            continue

        img = capture(args, name, ntsc, kind, settings, False, base)
        if not img:
            continue
        w, h, rgb, idx, colors, state = img
        if kind == "pal":
            ref = denise_yuv(w, h, idx, colors, settings, ntsc, 1 if state["first_line_odd"] else 0)
            meta = {"width": w, "height": h}
        else:
            meta_path = os.path.join(args.refs, name + ".json")
            if not os.path.exists(meta_path):
                check(f"{name}: reference", False, "missing")
                continue
            meta = json.load(open(meta_path))
            ref = open(os.path.join(args.refs, name + ".rgb"), "rb").read()
        if (w, h) != (meta["width"], meta["height"]):
            check(f"{name}: size", False, f"{w}x{h} vs {meta['width']}x{meta['height']}")
            continue
        diffs = [abs(a - b) for a, b in zip(rgb, ref)]
        maxd = max(diffs)
        mean = sum(diffs) / len(diffs)
        off = sum(1 for i in range(0, len(diffs), 3) if max(diffs[i:i + 3]) > 8)
        side = bytearray()
        for y in range(h):
            side += ref[y * w * 3:(y + 1) * w * 3] + rgb[y * w * 3:(y + 1) * w * 3]
        write_png(os.path.join(base, name + "-ref-new.png"), 2 * w, h, bytes(side))
        print(f"       mean {mean:.3f}, max {maxd}, pixels off > 8: {off} of {w * h}")
        limit = 1 if kind == "rgb" else 2
        check(f"{name}: within {limit}/255", maxd <= limit, f"max {maxd}")

    print(f"output in {base}")
    print(f"\n{mcplib.PASS} passed, {mcplib.FAIL} failed")
    sys.exit(1 if mcplib.FAIL else 0)


if __name__ == "__main__":
    main()
