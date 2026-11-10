#!/usr/bin/env python3
"""Checks rfxview-demo (engine side of viewer/PROTOCOL.md) from a fake app.
   python3 test_protocol.py path/to/rfxview-demo"""
import json, socket, struct, subprocess, sys, time

PORT = 18081
demo = subprocess.Popen([sys.argv[1], str(PORT)], stdout=subprocess.PIPE, text=True)
assert "listening" in demo.stdout.readline()
fails = 0

def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1

def connect():
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(3)
    return s

def send(s, t, payload=b"", slow=False):
    data = struct.pack("<HHI", t, 0, len(payload)) + payload
    if slow:                       # one byte at a time: the parser must reassemble
        for b in data:
            s.sendall(bytes([b])); time.sleep(0.001)
    else:
        s.sendall(data)

def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c:
            raise EOFError
        buf += c
    return buf

def recv(s):
    t, _, n = struct.unpack("<HHI", recv_exact(s, 8))
    return t, recv_exact(s, n)

def recv_until(s, want, limit=20):
    for _ in range(limit):
        t, p = recv(s)
        if t == want:
            return p
    raise AssertionError("no message %d" % want)

def frame(p):
    serial, sw, sh, x, y, w, h, fmt, comp = struct.unpack("<IHHHHHHBB", p[:18])
    return dict(serial=serial, sw=sw, sh=sh, x=x, y=y, w=w, h=h, fmt=fmt, comp=comp, px=p[20:])

W, H = 480, 696
s = connect()
send(s, 105, b"\0\0\x08\0")      # before HELLO: must be ignored
send(s, 100, struct.pack("<HHHHB", 1, W, H, 150, 0), slow=True)
t, p = recv(s)
check(t == 1 and struct.unpack("<H", p[:2])[0] == 1 and p[4:] == b"rfxengine", "WELCOME first")
st = json.loads(recv_until(s, 3))
check(st["url"] == "about:demo" and st["back"] is False, "initial STATE")
f = frame(recv_until(s, 2))
check((f["x"], f["y"], f["w"], f["h"], f["sw"], f["sh"]) == (0, 0, W, H, W, H), "first FRAME is the whole surface")
check(len(f["px"]) == W * H * 4 and f["fmt"] == 0, "BGRA payload size")
check(f["px"][3] == 0xFF and f["px"][3 + 4 * W * 20] == 0xFF, "pixels opaque")

# Flow control: no acks -> at most 2 frames in flight
for i in range(6):
    send(s, 104, struct.pack("<hh", 0, 10))
time.sleep(0.3)
s.settimeout(0.5)
frames = []
try:
    while True:
        t, p = recv(s)
        if t == 2:
            frames.append(frame(p))
except socket.timeout:
    pass
s.settimeout(3)
check(len(frames) == 1, "no more than 2 unacked frames (got %d more)" % len(frames))
send(s, 108, struct.pack("<I", frames[-1]["serial"]))
f2 = frame(recv_until(s, 2))
check(f2["serial"] == frames[-1]["serial"] + 1, "ACK releases the next frame")
send(s, 108, struct.pack("<I", f2["serial"]))

send(s, 999, b"future message")   # unknown type: skipped
send(s, 101, "https://example.org/".encode())
st = json.loads(recv_until(s, 3))
check(st["url"] == "https://example.org/" and st["back"] is True, "NAVIGATE -> STATE")
send(s, 103, struct.pack("<BBhh", 0, 0, 100, 50))
send(s, 103, struct.pack("<BBhh", 2, 0, 100, 50))
foc = recv_until(s, 4)
check(foc == b"\x01", "tap on the text field -> FOCUS 1")
send(s, 106, "héllo".encode())
st = json.loads(recv_until(s, 3))
check(st["title"] == "Demo: héllo", "TEXT (UTF-8) reaches the page")
send(s, 105, struct.pack("<BBH", 0, 0, 8))
st = json.loads(recv_until(s, 3))
check(st["title"] == "Demo: héll", "KEY backspace")
send(s, 102, b"\x01")
st = json.loads(recv_until(s, 3))
check(st["url"] == "about:demo" and st["forward"] is True, "COMMAND back")

# A second app connection replaces the first; RGB565 and RESIZE
s2 = connect()
send(s2, 100, struct.pack("<HHHHB", 1, 320, 240, 100, 1))
f = frame(recv_until(s2, 2))
check(f["fmt"] == 1 and len(f["px"]) == 320 * 240 * 2, "RGB565 frame")
send(s2, 108, struct.pack("<I", f["serial"]))
send(s2, 107, struct.pack("<HH", 240, 320))
f = frame(recv_until(s2, 2))
check((f["w"], f["h"]) == (240, 320), "RESIZE -> full frame at the new size")
try:
    s.settimeout(1)
    gone = s.recv(1) == b""
except (ConnectionResetError, socket.timeout):
    gone = True
check(gone, "old connection closed")

# Oversized length closes the connection
send(s2, 101, b"")
s2.sendall(struct.pack("<HHI", 101, 0, 64 * 1024 * 1024))
time.sleep(0.2)
try:
    s2.settimeout(1)
    while s2.recv(65536):
        pass
    closed = True
except (ConnectionResetError, socket.timeout):
    closed = False
check(closed, "oversized message closes the connection")

demo.kill()
print("protocol test:", "FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)
