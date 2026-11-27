#!/usr/bin/env python3
"""Smoke test for the VICE-compatible binary monitor (-binarymonitor).
Starts its own badline window instance (fresh XDG_CONFIG_HOME), sets an exec
checkpoint in the KERNAL keyboard wait loop, waits for the stop, then lists,
toggles, conditions and deletes checkpoints and resumes. Python stdlib only."""
import argparse, os, socket, struct, subprocess, sys, tempfile, time

STX, API = 0x02, 0x02
passed = failed = 0

def check(name, ok, info=""):
    global passed, failed
    if ok: passed += 1
    else: failed += 1
    print(("  ok   " if ok else "  FAIL ") + name + ("" if ok else "  -- " + str(info)))

class Mon:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.rid = 1
        self.buf = b""
    def recv_exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out
    def read(self):
        hdr = self.recv_exact(12)
        stx, api, blen, rtype, err, rid = struct.unpack("<BBIBBI", hdr)
        return rtype, err, rid, self.recv_exact(blen)
    def send(self, cmd, body=b""):
        rid = self.rid; self.rid += 1
        self.s.sendall(struct.pack("<BBIIB", STX, API, len(body), rid, cmd) + body)
        return rid
    def call(self, cmd, body=b"", want=None):
        """send and return the response for this request id (events are collected)"""
        rid = self.send(cmd, body)
        events = []
        while True:
            rtype, err, r, b = self.read()
            if r == rid and (want is None or rtype == want):
                return rtype, err, b, events
            events.append((rtype, err, r, b))
    def wait_type(self, rtype, timeout=10):
        self.s.settimeout(timeout)
        try:
            while True:
                t, e, r, b = self.read()
                if t == rtype:
                    return t, e, r, b
        finally:
            self.s.settimeout(10)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(os.path.dirname(__file__), "..", "builds", "release", "program", "badline"))
    ap.add_argument("--port", type=int, default=6512)
    args = ap.parse_args()
    cfgdir = tempfile.mkdtemp(prefix="badline-binmon-")
    env = dict(os.environ, XDG_CONFIG_HOME=cfgdir)
    proc = subprocess.Popen([args.binary, "-config-c64", os.path.join(cfgdir, "c64.ini"),
                             "-binarymonitor", "-binarymonitoraddress", "127.0.0.1:%d" % args.port],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        mon = None
        for _ in range(100):
            try:
                mon = Mon(args.port); break
            except OSError:
                time.sleep(0.1)
        check("connect", mon is not None)
        if not mon: return
        time.sleep(3)  # let BASIC reach READY.

        t, e, b, _ = mon.call(0x81)  # ping
        check("ping", e == 0, e)

        # exec checkpoint over the KERNAL GETIN/keyboard wait loop $E5CD-$E5D6
        body = struct.pack("<HHBBBB", 0xE5CD, 0xE5D6, 1, 1, 4, 0)
        t, e, b, ev = mon.call(0x12, body, want=0x11)
        check("checkpoint set", e == 0 and len(b) >= 4, e)
        num = struct.unpack_from("<I", b)[0] if b else 0

        # the CPU must stop there
        try:
            st = mon.wait_type(0x62, 5)
            pc = struct.unpack_from("<H", st[3])[0]
            check("stopped in wait loop", 0xE5CD <= pc <= 0xE5D6, hex(pc))
        except Exception as ex:
            check("stopped in wait loop", False, ex)

        t, e, b, ev = mon.call(0x31, b"\x00")  # registers
        check("registers while stopped", e == 0 and len(b) > 2, e)

        t, e, b, ev = mon.call(0x01, struct.pack("<BHHB", 0, 0xA000, 0xA003, 0) + b"\x00\x00")
        check("memory get", e == 0 and len(b) >= 6, (e, b[:8]))

        # second checkpoint: store watchpoint, created disabled
        t, e, b, ev = mon.call(0x12, struct.pack("<HHBBBB", 0xC000, 0xC000, 1, 0, 2, 0), want=0x11)
        check("watchpoint set disabled", e == 0, e)
        num2 = struct.unpack_from("<I", b)[0] if b else 0

        t, e, b, ev = mon.call(0x14, want=0x14)  # list
        infos = [x for x in ev if x[0] == 0x11]
        check("list counts 2", e == 0 and struct.unpack_from("<I", b)[0] == 2 and len(infos) == 2, (e, b, len(infos)))

        cond = b"REGA == $ff"
        t, e, b, ev = mon.call(0x22, struct.pack("<IB", num, len(cond)) + cond)
        check("condition set", e == 0, e)
        t, e, b, ev = mon.call(0x22, struct.pack("<IB", num2, 7) + b"REGA ==")
        check("invalid condition fails", e == 0x8f, e)


        t, e, b, ev = mon.call(0x15, struct.pack("<IB", num2, 1))
        check("toggle enable", e == 0, e)

        t, e, b, ev = mon.call(0x13, struct.pack("<I", num))
        check("delete checkpoint", e == 0, e)
        t, e, b, ev = mon.call(0x13, struct.pack("<I", num2))
        check("delete watchpoint", e == 0, e)
        t, e, b, ev = mon.call(0x13, struct.pack("<I", 999))
        check("delete unknown fails", e == 0x01, e)

        t, e, b, ev = mon.call(0x14, want=0x14)
        check("list empty", e == 0 and struct.unpack_from("<I", b)[0] == 0, b)

        t, e, b, ev = mon.call(0xaa)  # exit = resume
        check("exit resumes", e == 0, e)
        got = any(x[0] == 0x63 for x in ev)
        if not got:
            try:
                mon.wait_type(0x63, 3); got = True
            except Exception:
                pass
        check("resumed event", got)
        time.sleep(1)
        # no new stop after deleting the checkpoint
        mon.s.settimeout(1.5)
        stopped = False
        try:
            while True:
                t, e, r, b = mon.read()
                if t == 0x62: stopped = True; break
        except (socket.timeout, TimeoutError):
            pass
        check("runs after delete", not stopped)
        check("process alive", proc.poll() is None, proc.poll())
        mon.call(0xbb)  # quit
        try:
            proc.wait(10)
            check("quit", True)
        except subprocess.TimeoutExpired:
            check("quit", False, "timeout")
    finally:
        if proc.poll() is None:
            proc.kill()
    print("\n%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)

main()
