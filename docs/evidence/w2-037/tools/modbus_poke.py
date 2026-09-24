#!/usr/bin/env python3
"""w2-037 test helper (copied from docs/evidence/w2-036/tools): minimal Modbus TCP client for Adam60xxSimulator ONLY.

Used to put the simulator into a known, non-default state before the desktop app starts
(D5) and to change the ADAM-6022 AO0 value while the app runs (D1), and to read values
back as Modbus evidence.  No third-party packages (raw Modbus TCP over a socket).

Safety: the host must be one of 127.0.0.201..205 (the simulator profile endpoints).
Any other host - in particular the plant addresses 192.168.1.x - is refused (exit 2).

Every call appends one timestamped line to the input log (default: input-log.txt next to
this file's parent evidence folder), so all non-app writes to the simulator are recorded.

  python modbus_poke.py read-hr   HOST START COUNT
  python modbus_poke.py write-hr  HOST ADDR VALUE
  python modbus_poke.py read-coils HOST START COUNT
  python modbus_poke.py write-coil HOST ADDR 0|1
  python modbus_poke.py read-di   HOST START COUNT
Options (before the command): --note "text"  --log PATH
"""
from __future__ import annotations

import datetime as _dt
import os
import socket
import struct
import sys

ALLOWED_HOSTS = {f"127.0.0.{n}" for n in range(201, 206)}
UNIT_ID = 1
_tid = 0


def _now() -> str:
    return _dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def _request(host: str, pdu: bytes) -> bytes:
    global _tid
    _tid = (_tid + 1) & 0xFFFF
    mbap = struct.pack(">HHHB", _tid, 0, len(pdu) + 1, UNIT_ID)
    with socket.create_connection((host, 502), timeout=3.0) as s:
        s.sendall(mbap + pdu)
        head = b""
        while len(head) < 7:
            chunk = s.recv(7 - len(head))
            if not chunk:
                raise ConnectionError("connection closed")
            head += chunk
        _, _, length, _ = struct.unpack(">HHHB", head)
        body = b""
        while len(body) < length - 1:
            chunk = s.recv(length - 1 - len(body))
            if not chunk:
                raise ConnectionError("connection closed")
            body += chunk
    if body[0] & 0x80:
        raise RuntimeError(f"Modbus exception function=0x{body[0]:02x} code={body[1]}")
    return body


def read_registers(host: str, start: int, count: int) -> list[int]:
    body = _request(host, struct.pack(">BHH", 0x03, start, count))
    n = body[1]
    return list(struct.unpack(">" + "H" * (n // 2), body[2:2 + n]))


def read_bits(host: str, fc: int, start: int, count: int) -> list[int]:
    body = _request(host, struct.pack(">BHH", fc, start, count))
    data = body[2:2 + body[1]]
    return [(data[i // 8] >> (i % 8)) & 1 for i in range(count)]


def write_register(host: str, addr: int, value: int) -> None:
    _request(host, struct.pack(">BHH", 0x06, addr, value))


def write_coil(host: str, addr: int, on: bool) -> None:
    _request(host, struct.pack(">BHH", 0x05, addr, 0xFF00 if on else 0x0000))


def main(argv: list[str]) -> int:
    note = ""
    log = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "input-log.txt")
    while len(argv) > 1 and argv[1].startswith("--"):
        if argv[1] == "--note":
            note, argv = argv[2], argv[:1] + argv[3:]
        elif argv[1] == "--log":
            log, argv = argv[2], argv[:1] + argv[3:]
        else:
            print(f"unknown option {argv[1]}")
            return 1
    if len(argv) != 5:
        print(__doc__)
        return 1
    cmd, host, a, b = argv[1], argv[2], int(argv[3]), int(argv[4])
    if host not in ALLOWED_HOSTS:
        print(f"REFUSED: host {host} is not a simulator endpoint (127.0.0.201..205)")
        return 2

    if cmd == "read-hr":
        result = f"HR{a}..{a + b - 1} = {read_registers(host, a, b)}"
    elif cmd == "read-coils":
        result = f"coil{a}..{a + b - 1} = {read_bits(host, 0x01, a, b)}"
    elif cmd == "read-di":
        result = f"DI{a}..{a + b - 1} = {read_bits(host, 0x02, a, b)}"
    elif cmd == "write-hr":
        write_register(host, a, b)
        result = f"wrote HR{a} = {b}; read back {read_registers(host, a, 1)}"
    elif cmd == "write-coil":
        write_coil(host, a, b != 0)
        result = f"wrote coil{a} = {1 if b else 0}; read back {read_bits(host, 0x01, a, 1)}"
    else:
        print(f"unknown command {cmd}")
        return 1

    line = f"[{_now()}] modbus_poke {cmd} {host} {a} {b}: {result}" + (f"  ({note})" if note else "")
    print(line)
    with open(log, "a", encoding="utf-8") as f:
        f.write(line + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

