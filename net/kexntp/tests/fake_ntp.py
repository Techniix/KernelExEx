#!/usr/bin/env python3
"""Minimal NTP server for testing kexntp: answers with the real time plus
an offset (seconds, argv[2]) on 127.0.0.1:argv[1]."""
import socket, struct, sys, time
port = int(sys.argv[1]) if len(sys.argv) > 1 else 123
skew = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
def stamp(t):
    t += 2208988800
    return struct.pack("!II", int(t), int((t % 1) * 2**32))
while True:
    data, addr = s.recvfrom(512)
    if len(data) < 48:
        continue
    now = time.time() + skew
    reply = bytes([0x24, 2, 6, 0xEC]) + b"\0" * 8 + b"LOCL" + stamp(now) + data[40:48] + stamp(now) + stamp(now)
    s.sendto(reply, addr)
