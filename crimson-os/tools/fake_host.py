#!/usr/bin/env python3
"""Minimal stand-in for crimson-host, for headless tests.

Listens on TCP 7700, prints each partition's HELLO, sends it a layout
(Game OS: full + focus, System OS: guide overlay), and writes the partition's
first complete frame to <role>-frame.ppm. No dependencies.
"""
import socket, struct, sys, threading, time

MAGIC = 0x534D5243
ROLES = {1: "game", 2: "system"}


def recv_all(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def serve(conn, addr):
    hdr = recv_all(conn, 12)
    magic, typ, ver, ln = struct.unpack("<IHHI", hdr)
    role, w, h = struct.unpack("<III", recv_all(conn, 12))
    name = recv_all(conn, ln - 12).split(b"\0")[0].decode(errors="replace")
    r = ROLES.get(role, "?")
    print(f"{time.strftime('%H:%M:%S')} HELLO from {addr[0]}: {r} partition '{name}' {w}x{h}", flush=True)
    loc = 1 if role == 1 else 4
    conn.sendall(struct.pack("<IHHI", MAGIC, 4, 1, 16) + struct.pack("<IIII", loc, 1, 1280, 720))
    buf, frames, t0, saved = None, 0, time.time(), False
    while True:
        magic, typ, ver, ln = struct.unpack("<IHHI", recv_all(conn, 12))
        body = recv_all(conn, ln)
        if typ != 2:
            continue
        fw, fh, y0, rows, seq = struct.unpack("<IIIII", body[:20])
        if buf is None or len(buf) != fw * fh * 4:
            buf = bytearray(fw * fh * 4)
        buf[y0 * fw * 4:(y0 + rows) * fw * 4] = body[20:]
        frames += 1
        if not saved and y0 == 0 and rows == fh:
            rgb = bytearray(fw * fh * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = buf[2::4], buf[1::4], buf[0::4]
            with open(f"{r}-frame.ppm", "wb") as f:
                f.write(b"P6 %d %d 255\n" % (fw, fh) + rgb)
            print(f"  saved {r}-frame.ppm ({fw}x{fh})", flush=True)
            saved = True
        if time.time() - t0 >= 10:
            print(f"  {r}: {frames / (time.time() - t0):.1f} updates/s", flush=True)
            frames, t0 = 0, time.time()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7700
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", port))
    ls.listen()
    print(f"fake crimson host on :{port}", flush=True)
    while True:
        c, a = ls.accept()
        threading.Thread(target=lambda: _safe(serve, c, a), daemon=True).start()


def _safe(f, *a):
    try:
        f(*a)
    except EOFError:
        print("  partition disconnected", flush=True)


if __name__ == "__main__":
    main()
