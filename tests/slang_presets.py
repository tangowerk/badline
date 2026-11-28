#!/usr/bin/env python3
"""Slang preset check (M-VK3a): reference presets on the Vulkan renderer.

  tests/slang_presets.py [--binary target/debug/badline] [--port 6467] [--shaders DIR]
                         [--out DIR] [--only SUBSTRING] [--cold]

Starts a Vulkan window instance (own XDG_CONFIG_HOME, fresh C64 config, nearest filter,
integer scaling), shows a static picture with all 16 colours and for every reference preset:
loads it through the MCP `shader` tool, runs frames (history/feedback advance), takes a rendered
screenshot (saved to --out for a visual check) and checks it is not black and differs from the
plain picture. Afterwards: parameter change, shader removal restores the plain picture
bit-identically. Vulkan validation messages on stderr fail the run (dev builds enable the layer
when it is installed). --cold uses an empty shader cache (XDG_CACHE_HOME in the temp dir).
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcplib import Client, check, compare, decode_png, image_of, text_of  # noqa: E402
import mcplib  # noqa: E402

# (preset relative to the shader directory, feature it covers)
PRESETS = [
    ("crt/crt-geom.slangp", "single pass, parameters"),
    ("crt/crt-lottes.slangp", "single pass"),
    ("crt/crt-royale.slangp", "LUTs, mipmaps, float + sRGB FBOs, aliases"),
    ("ntsc/ntsc-adaptive.slangp", "absolute/source scales"),
    ("motionblur/motionblur-simple.slangp", "OriginalHistory"),
    ("motionblur/feedback.slangp", "PassFeedback"),
    ("test/history.slangp", "history (RetroArch test)"),
    ("test/feedback.slangp", "feedback (RetroArch test)"),
    ("test/format.slangp", "#pragma format"),
    ("test/pragma-name.slangp", "#pragma name aliases"),
    ("test/frame_count.slangp", "frame_count_mod"),
    ("crt/shaders/crt-yah/presets/pure-crt.slangp", "#reference + parameter overrides"),
    # MBZ__5__POTATO reads an unwritten vertex output (corpus bug, validation error)
    ("bezel/Mega_Bezel/Presets/MBZ__3__STD-GLASS.slangp", "Mega Bezel"),
]


def shot(client, label, out):
    r = client.call("screenshot", rendered=True)
    png = image_of(r)
    if png is None:
        check(f"{label}: rendered screenshot", False, text_of(r))
        return None
    with open(os.path.join(out, label.replace("/", "_") + ".png"), "wb") as f:
        f.write(png)
    return decode_png(png)


def not_black(img):
    w, h, ch, px = img
    return any(px[i] > 8 for i in range(0, len(px), 7))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="target/debug/badline")
    ap.add_argument("--port", type=int, default=6467)
    ap.add_argument("--shaders", default="/usr/share/libretro/shaders/shaders_slang")
    ap.add_argument("--out", default=None)
    ap.add_argument("--only", default="")
    ap.add_argument("--cold", action="store_true")
    args = ap.parse_args()

    base = tempfile.mkdtemp(prefix="badline-slang-")
    out = args.out or os.path.join(base, "shots")
    os.makedirs(out, exist_ok=True)
    cfg = os.path.join(base, "c64.ini")
    with open(cfg, "w") as f:
        f.write("video_filter:0\ninteger_scaling:1\n")
    env = dict(os.environ, XDG_CONFIG_HOME=base, BADLINE_VK_VALIDATION="1")
    if args.cold:
        env["XDG_CACHE_HOME"] = os.path.join(base, "cache")
    errlog = open(os.path.join(base, "stderr.txt"), "w+")
    proc = subprocess.Popen([args.binary, "-config-c64", cfg, "-mcp", str(args.port)],
                            env=env, stdout=subprocess.DEVNULL, stderr=errlog, text=True)
    client = Client(args.port)
    try:
        for _ in range(100):
            try:
                client.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "slang", "version": "1"}})
                break
            except OSError:
                time.sleep(0.1)
        client.call("reset", type="hard")
        r = client.call("wait_for_text", text="READY.", timeout=10, warp=True)
        check("READY.", not r.get("isError"), text_of(r))
        # frozen cursor: the IRQ skips keyboard scan and cursor blink ($ea81 = end of the KERNAL IRQ)
        client.call("memory_write", address=0x0314, bytes=[0x81, 0xea])
        for i in range(16):
            client.call("memory_write", address=0xd800 + 200 + i, bytes=[i], bank="cpu")
            client.call("memory_write", address=0x0400 + 200 + i, bytes=[0xa0])
        client.call("run_frames", frames=5)
        # paused: run_frames advances exact frames (history/feedback inputs are deterministic)
        client.call("pause")
        plain = shot(client, "plain", out)

        for rel, what in PRESETS:
            if args.only and args.only not in rel:
                continue
            path = os.path.join(args.shaders, rel)
            t0 = time.time()
            r = client.call("shader", load=path)
            dt = time.time() - t0
            if r.get("isError"):
                check(f"{rel} ({what}) loads", False, text_of(r)[:300])
                continue
            s = json.loads(text_of(r))
            print(f"  {rel}: {s['passes']} passes, {s['luts']} LUTs, history {s['history']}, "
                  f"feedback {s['feedback_passes']}, {len(s['parameters'])} params, "
                  f"{s['compiled_stages']} compiled / {s['cached_stages']} cached, load {s['load_ms']} ms, "
                  f"total {dt:.1f} s" + (f", warnings: {s['warnings'][:2]}" if s["warnings"] else ""))
            client.call("run_frames", frames=4)
            # history/feedback need a changing picture: border colour per frame, back to 14
            for k in (2, 3, 4, 5, 6, 7, 14):
                client.call("memory_write", address=0xd020, bytes=[k], bank="cpu")
                client.call("run_frames", frames=1)
            img = shot(client, rel, out)
            if img and plain:
                same, diff, _ = compare(plain, img)
                check(f"{rel} ({what}) renders", not_black(img) and (not same or diff > 0),
                      "black" if not not_black(img) else "identical to the plain picture")

        if not args.only:
            # parameter change re-runs the passes while paused
            client.call("shader", load=os.path.join(args.shaders, "crt/crt-geom.slangp"))
            client.call("run_frames", frames=2)
            a = shot(client, "geom_default", out)
            r = client.call("shader", set_param="CRTgamma", value=1.5)
            check("set_param", not r.get("isError"), text_of(r)[:200])
            time.sleep(0.3)
            b = shot(client, "geom_gamma", out)
            if a and b:
                same, diff, _ = compare(a, b)
                check("parameter changes the paused picture", same and diff > 0)
            r = client.call("shader", clear=True)
            check("clear", not r.get("isError"), text_of(r))
            client.call("run_frames", frames=3)
            back = shot(client, "plain_again", out)
            if plain and back:
                same, diff, maxd = compare(plain, back)
                check("plain picture restored bit-identically", same and diff == 0, f"{diff} pixels, max {maxd}")
    finally:
        proc.send_signal(15)
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
    errlog.seek(0)
    errors = [l.rstrip() for l in errlog if "validation" in l]
    check("no Vulkan validation messages", not errors, "\n    " + "\n    ".join(errors[:8]))
    print(f"screenshots: {out}")
    print(f"{mcplib.PASS} ok, {mcplib.FAIL} failed")
    return 1 if mcplib.FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
