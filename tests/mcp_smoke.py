#!/usr/bin/env python3
"""Smoke test for the embedded MCP server (standard library only).

Usage:
  tests/mcp_smoke.py [--binary builds/release/program/denise] [--port 6465] [--attach]

Without --attach the script starts the emulator with -mcp <port>, runs all
tools once and kills the emulator afterwards. With --attach it talks to an
already running instance on the given port.
"""

import argparse
import base64
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PASS = 0
FAIL = 0


class Client:
    def __init__(self, port):
        self.url = f"http://127.0.0.1:{port}/mcp"
        self.next_id = 1

    def rpc(self, method, params=None, timeout=120):
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        self.next_id += 1
        if params is not None:
            msg["params"] = params
        req = urllib.request.Request(self.url, data=json.dumps(msg).encode(),
                                     headers={"Content-Type": "application/json",
                                              "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())

    def call(self, name, **args):
        resp = self.rpc("tools/call", {"name": name, "arguments": args})
        if "error" in resp:
            raise RuntimeError(f"{name}: {resp['error']}")
        return resp["result"]


def text_of(result):
    return "\n".join(c["text"] for c in result["content"] if c["type"] == "text")


def check(label, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok   {label}")
    else:
        FAIL += 1
        print(f"  FAIL {label} {detail}")


def basic_prg(lines):
    """Tokenize trivial BASIC lines of the form (number, [bytes]) into a PRG."""
    addr = 0x0801
    body = bytearray()
    for number, content in lines:
        line = bytearray(number.to_bytes(2, "little")) + bytearray(content) + b"\x00"
        addr_next = addr + 2 + len(line)
        body += addr_next.to_bytes(2, "little") + line
        addr = addr_next
    body += b"\x00\x00"
    return b"\x01\x08" + bytes(body)


def wait_for_server(client, proc, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        if proc and proc.poll() is not None:
            raise RuntimeError("emulator exited early")
        try:
            client.rpc("ping", timeout=2)
            return
        except (urllib.error.URLError, ConnectionError, OSError):
            time.sleep(0.2)
    raise RuntimeError("MCP server not reachable")


def png_size(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def run(client, tmp):
    print("protocol")
    init = client.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                     "clientInfo": {"name": "smoke", "version": "1"}})
    check("initialize", init["result"]["serverInfo"]["name"] == "underground64")
    tools = {t["name"] for t in client.rpc("tools/list")["result"]["tools"]}
    expected = {"status", "reset", "pause", "resume", "warp", "load_media", "eject", "savestate_save",
                "savestate_load", "type_text", "key", "joystick", "run_frames", "wait_for_text",
                "read_screen", "screenshot"}
    check("tools/list", expected <= tools, str(expected - tools))
    unknown = client.rpc("tools/call", {"name": "nope", "arguments": {}})
    check("unknown tool -> error", "error" in unknown)

    print("boot")
    client.call("reset", type="hard")
    r = client.call("wait_for_text", text="READY.", timeout=10, warp=True)
    check("READY. after hard reset", not r["isError"], text_of(r))
    screen = text_of(client.call("read_screen"))
    check("read_screen shows BASIC banner", "COMMODORE 64 BASIC V2" in screen, screen[:200])
    raw = text_of(client.call("read_screen", raw=True))
    check("read_screen raw", len(raw.splitlines()) == 25 and len(raw.splitlines()[0].split()) == 40)

    print("input")
    r = client.call("type_text", text="PRINT 6*7\n")
    check("type_text", not r["isError"], text_of(r))
    r = client.call("wait_for_text", text=" 42", timeout=2)
    check("typed command executed", not r["isError"], text_of(r))
    client.call("key", keys=["Left Shift", "Home"], frames=3)
    client.call("run_frames", frames=5)
    screen = text_of(client.call("read_screen"))
    check("key Shift+Home clears screen", "COMMODORE" not in screen and "42" not in screen, screen[:120])
    r = client.call("key", keys=["NoSuchKey"])
    check("unknown key -> isError", r["isError"])
    r = client.call("joystick", port=2, up=True, fire=True, frames=3)
    check("joystick", not r["isError"] or "No joystick" in text_of(r), text_of(r))

    print("pause / frames")
    client.call("pause")
    f0 = json.loads(text_of(client.call("status")))["frame"]
    client.call("run_frames", frames=10)
    st = json.loads(text_of(client.call("status")))
    check("run_frames steps exactly while paused", st["frame"] - f0 == 10 and st["paused"], f"{f0} -> {st['frame']}")
    time.sleep(0.3)
    st2 = json.loads(text_of(client.call("status")))
    check("stays paused", st2["frame"] == st["frame"])
    client.call("resume")
    time.sleep(0.3)
    st3 = json.loads(text_of(client.call("status")))
    check("resume runs", st3["frame"] > st2["frame"] and not st3["paused"])
    client.call("warp", enabled=True)
    check("warp on", json.loads(text_of(client.call("status")))["warp"])
    client.call("warp", enabled=False)
    check("warp off", not json.loads(text_of(client.call("status")))["warp"])
    r = client.call("run_frames", frames=200, warp=True)
    check("run_frames warp restores warp", not json.loads(text_of(client.call("status")))["warp"], text_of(r))

    print("screenshot")
    for border, size in (("none", (320, 200)), ("visible", None), ("full", None)):
        r = client.call("screenshot", border=border, scale=1)
        imgs = [c for c in r["content"] if c["type"] == "image"]
        dims = png_size(base64.b64decode(imgs[0]["data"])) if imgs else None
        ok = dims is not None and (size is None or dims == size)
        check(f"screenshot border={border} {dims}", ok, text_of(r))
    r = client.call("screenshot", border="none", scale=2)
    imgs = [c for c in r["content"] if c["type"] == "image"]
    check("screenshot scale=2", imgs and png_size(base64.b64decode(imgs[0]["data"])) == (640, 400))
    r = client.call("screenshot", rendered=True)
    imgs = [c for c in r["content"] if c["type"] == "image"]
    check("screenshot rendered", bool(imgs) and png_size(base64.b64decode(imgs[0]["data"])) is not None, text_of(r))

    print("media")
    # 10 PRINT "MCP OK"
    prg = basic_prg([(10, [0x99] + list(b' "MCP OK"'))])
    prg_path = os.path.join(tmp, "smoke.prg")
    with open(prg_path, "wb") as f:
        f.write(prg)
    r = client.call("load_media", path=prg_path)
    check("load_media path", not r["isError"], text_of(r))
    r = client.call("wait_for_text", text="MCP OK", timeout=15, warp=True)
    check("PRG autostarted (path)", not r["isError"], text_of(r))
    client.call("reset", type="hard")
    client.call("wait_for_text", text="READY.", timeout=10, warp=True)
    r = client.call("load_media", data_base64=base64.b64encode(prg).decode(), filename="smoke2.prg")
    check("load_media base64", not r["isError"], text_of(r))
    r = client.call("wait_for_text", text="MCP OK", timeout=15, warp=True)
    check("PRG autostarted (base64)", not r["isError"], text_of(r))
    r = client.call("load_media", path="/nonexistent.prg")
    check("missing file -> isError", r["isError"])
    r = client.call("eject", target="all")
    check("eject", not r["isError"], text_of(r))

    print("savestates")
    client.call("type_text", text="A=4711\n")
    client.call("run_frames", frames=10)
    state = os.path.join(tmp, "smoke.state")
    r = client.call("savestate_save", path=state)
    check("savestate_save", not r["isError"] and os.path.getsize(state) > 0, text_of(r))
    client.call("reset", type="hard")
    client.call("wait_for_text", text="READY.", timeout=10, warp=True)
    r = client.call("savestate_load", path=state)
    check("savestate_load", not r["isError"], text_of(r))
    client.call("type_text", text="PRINT A\n")
    r = client.call("wait_for_text", text=" 4711", timeout=2)
    check("state restored variable", not r["isError"], text_of(r))

    print("timeouts")
    r = client.call("wait_for_text", text="NEVER THERE", timeout=1)
    check("wait_for_text timeout -> isError", r["isError"] and "Timeout" in text_of(r))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "..", "builds", "release", "program", "denise"))
    ap.add_argument("--port", type=int, default=6465)
    ap.add_argument("--attach", action="store_true")
    args = ap.parse_args()

    client = Client(args.port)
    proc = None
    if not args.attach:
        # fresh C64 config, so stale media paths from the user config cannot pop up dialogs
        cfgdir = tempfile.mkdtemp(prefix="u64-smoke-")
        proc = subprocess.Popen([args.binary, "-config-c64", os.path.join(cfgdir, "c64.ini"), "-mcp", str(args.port)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_server(client, proc)
        with tempfile.TemporaryDirectory() as tmp:
            run(client, tmp)
    finally:
        if proc:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()

    print(f"\n{PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
