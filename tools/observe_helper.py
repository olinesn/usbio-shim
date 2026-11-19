#!/usr/bin/env python3
"""
Observation helper: pretends one UltraSoundGate (0547:<pid>) is attached and logs every
request the shim forwards. IOCTLs get the canned answers in ANSWERS (by IOCTL code), else
an error, so we can see how far RECORDER USGH gets and what it asks for. No USB access.

    python3 -I observe_helper.py [--pid 0x1012] [--port 47321] [--answers answers.py]
"""
import argparse
import socket
import struct
import threading
import time

HDR = struct.Struct('<9I')   # magic op id arg arg2 len error bytes flags
MAGIC = 0x4F494253
OPS = {1: 'HELLO', 2: 'LIST', 3: 'OPEN', 4: 'IOCTL', 5: 'READ', 6: 'WRITE', 7: 'CANCEL'}
ERROR_NOT_SUPPORTED = 50

ANSWERS = {}   # code -> bytes, or callable(inbuf, outlen) -> (error, bytes)


def recv_all(s, n):
    b = b''
    while len(b) < n:
        k = s.recv(n - len(b))
        if not k:
            raise ConnectionError
        b += k
    return b


def serve(conn, n, args, t0):
    def log(msg):
        print(f"{time.time() - t0:8.3f} c{n} {msg}", flush=True)
    try:
        while True:
            magic, op, rid, arg, arg2, ln, *_ = HDR.unpack(recv_all(conn, HDR.size))
            pay = recv_all(conn, ln) if ln else b''
            name = OPS.get(op, op)
            if op == 2:
                if arg == args.pid:
                    dev = struct.pack('<HHIB7s64s', 0x0547, arg, 0, 1, b'\x01', b'FAKE0001')
                    log(f"LIST pid {arg:04x} -> 1 device")
                else:
                    dev = b''
                conn.sendall(HDR.pack(MAGIC, op, rid, 0, 0, len(dev), 0, 0, 0) + dev)
            elif op == 3:
                log(f"OPEN pid {arg:04x} index {arg2}")
                conn.sendall(HDR.pack(MAGIC, op, rid, 0, 0, 0, 0, 0, 0))
            elif op == 4:
                ans = ANSWERS.get(arg)
                if callable(ans):
                    err, out = ans(pay, arg2)
                elif ans is not None:
                    err, out = 0, ans[:arg2]
                else:
                    err, out = ERROR_NOT_SUPPORTED, b''
                fn = (arg >> 2) & 0xFFF
                log(f"IOCTL {arg:#010x} (function {fn:#x}, method {arg & 3}) in={pay.hex()} outlen={arg2} -> err {err} out={out.hex()}")
                conn.sendall(HDR.pack(MAGIC, op, rid, 0, 0, len(out), err, len(out), 0) + out)
            else:
                log(f"{name} arg={arg} payload={pay[:64].hex()}")
                conn.sendall(HDR.pack(MAGIC, op, rid, 0, 0, 0, ERROR_NOT_SUPPORTED, 0, 0))
    except ConnectionError:
        log("closed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=lambda x: int(x, 0), default=0x1012)
    ap.add_argument('--port', type=int, default=47321)
    ap.add_argument('--answers', help='python file defining ANSWERS = {code: bytes|callable}')
    args = ap.parse_args()
    if args.answers:
        ns = {}
        exec(open(args.answers).read(), ns)
        ANSWERS.update(ns['ANSWERS'])
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', args.port))
    srv.listen(16)
    t0 = time.time()
    n = 0
    while True:
        c, _ = srv.accept()
        n += 1
        threading.Thread(target=serve, args=(c, n, args, t0), daemon=True).start()


if __name__ == '__main__':
    main()
