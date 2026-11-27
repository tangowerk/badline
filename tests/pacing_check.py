#!/usr/bin/env python3
"""Frame pacing check (M-Pacing1) via the MCP `pacing` tool.

Starts a Vulkan window instance (own XDG_CONFIG_HOME, own port) unless --attach is given,
optionally autostarts a PRG (default: tests/pacing/softscroll.prg), resets the pacing
statistics, waits --seconds and evaluates them.

Acceptance (SPEC "Decisions M-Pacing"): under video master 0 repeated/shortened/skipped
C64 frames and no audio underflows/drops during the measurement.

    tests/pacing_check.py --seconds 60
    tests/pacing_check.py --attach --port 6464 --seconds 60
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from mcp_smoke import Client, text_of, wait_for_server  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def move_to_monitor(pid, monitor):
    """Hyprland only: moves the window of `pid` to the active workspace of `monitor`
    (the pacing decision follows via the display-changed event)."""
    clients = json.loads(subprocess.check_output(["hyprctl", "clients", "-j"]))
    monitors = json.loads(subprocess.check_output(["hyprctl", "monitors", "-j"]))
    addr = next(c["address"] for c in clients if c["pid"] == pid)
    ws = next(m["activeWorkspace"]["id"] for m in monitors if m["name"] == monitor)
    subprocess.run(["hyprctl", "dispatch",
                    f"hl.dsp.window.move({{ workspace = '{ws}', follow = false, window = 'address:{addr}' }})"],
                   capture_output=True)
    time.sleep(2)
    clients = json.loads(subprocess.check_output(["hyprctl", "clients", "-j"]))
    names = {m["id"]: m["name"] for m in monitors}
    on = next(names.get(c["monitor"]) for c in clients if c["pid"] == pid)
    if on != monitor:
        raise RuntimeError(f"window is on {on}, not {monitor}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(HERE, "..", "target", "release", "badline"))
    ap.add_argument("--port", type=int, default=6475)
    ap.add_argument("--attach", action="store_true")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--settle", type=float, default=3, help="seconds before the measurement starts")
    ap.add_argument("--prg", default=os.path.join(HERE, "pacing", "softscroll.prg"))
    ap.add_argument("--mode", choices=["auto", "lowest_latency"])
    ap.add_argument("--fullscreen", action="store_true")
    ap.add_argument("--ntsc", action="store_true", help="NTSC VIC-II (6567R8)")
    ap.add_argument("--monitor", help="Hyprland: move the test window to this monitor (e.g. eDP-1)")
    ap.add_argument("--json", action="store_true", help="print the raw stats")
    ap.add_argument("--save", help="write the raw stats to this file")
    args = ap.parse_args()

    client = Client(args.port)
    proc = None
    if not args.attach:
        cfgdir = tempfile.mkdtemp(prefix="badline-pacing-")
        env = dict(os.environ, XDG_CONFIG_HOME=cfgdir)
        cmd = [args.binary, "-config-c64", os.path.join(cfgdir, "c64.ini"), "-renderer", "vulkan",
               "-mcp", str(args.port)]
        if args.fullscreen:
            cmd.append("-fullscreen")
        if args.ntsc:
            cmd.append("-vic-6567R8")
        proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_server(client, proc)
        if args.monitor and proc:
            move_to_monitor(proc.pid, args.monitor)
        if args.mode:
            client.call("pacing", set_mode=args.mode)
        if args.prg and os.path.exists(args.prg):
            client.call("load_media", path=os.path.abspath(args.prg))
        time.sleep(args.settle)
        client.call("pacing", action="reset")
        time.sleep(args.seconds)
        stats = json.loads(text_of(client.call("pacing", action="stats")))
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()

    if args.json:
        print(json.dumps(stats, indent=1))
    if args.save:
        with open(args.save, "w") as fh:
            json.dump(stats, fh, indent=1)
    v, a = stats.get("video", {}), stats.get("audio", {})
    f = v.get("frames", {})
    print(f"state {stats['state']} repeat {stats['repeat']} depth {stats.get('queue_depth')} "
          f"refresh {stats['refresh_hz']} Hz emu {stats['emulation_fps']} fps feedback {stats['feedback']}")
    print(f"{v.get('seconds')} s, {v.get('presents')} presents ({v.get('presents_without_time')} without time), "
          f"intervals {v.get('present_intervals')}")
    print(f"frames {f.get('count')}: on screen {f.get('periods_on_screen')} expected {f.get('expected_periods')}, "
          f"repeated {f.get('repeated')} shortened {f.get('shortened')} skipped {f.get('skipped')}")
    print(f"present->display avg {v.get('present_to_display_ms_avg')} ms max {v.get('present_to_display_ms_max')} ms")
    print(f"audio: underflows {a.get('underflows')} dropped {a.get('dropped_bytes')} B, drc {a.get('drc')}, "
          f"deviation {a.get('buffer_deviation_min')}..{a.get('buffer_deviation_max')} "
          f"(avg {a.get('buffer_deviation_avg')}), rate adjust {a.get('rate_adjust_min_percent')}.."
          f"{a.get('rate_adjust_max_percent')} %")

    if stats["state"] != "video_master":
        print("not video master: no acceptance check")
        return 0
    ok = (f.get("repeated") == 0 and f.get("shortened") == 0 and f.get("skipped") == 0
          and a.get("underflows") == 0 and a.get("dropped_bytes") == 0)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
