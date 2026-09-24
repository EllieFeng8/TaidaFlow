"""w2-043: minimal TCP/WebSocket probe for the desktop LAN relay (no GUI interaction).

Copied from taidaflow-main/build/w2-042-tools/ws_probe.py (w2-042, main branch worktree);
the "occupy" mode takes an optional bind address (w2-043 uses it for the safety_probe
self-test on 127.0.0.1:18125).

Usage:
  python -B ws_probe.py connect <host> <port> <origin> <hold_seconds>
      Opens TCP, sends a WebSocket upgrade for /mirror with the given Origin, prints the HTTP
      status line, then keeps the connection open <hold_seconds> and reports whether the
      server sent a Close frame (a rejected Origin gets Close 1008 right after the 101).
      Writes "HOLDING" to stdout once the handshake is done (the caller samples netstat then).
  python -B ws_probe.py refused <host> <port>
      Expects the TCP connect to fail (prints REFUSED) - used for the internal port 18125
      on the LAN address.
  python -B ws_probe.py occupy <port> <seconds> [bind_address]
      Listens on <bind_address>:<port> (default 0.0.0.0) for <seconds>.
"""
import base64
import os
import socket
import struct
import sys
import time


def connect(host, port, origin, hold):
    key = base64.b64encode(os.urandom(16)).decode()
    s = socket.create_connection((host, port), timeout=5)
    local = s.getsockname()
    print(f"TCP_ESTABLISHED local={local[0]}:{local[1]} remote={host}:{port}", flush=True)
    req = (
        f"GET /mirror HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
        f"Origin: {origin}\r\n\r\n"
    )
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    status = head.split(b"\r\n", 1)[0].decode(errors="replace")
    print(f"HTTP_STATUS {status}", flush=True)
    print("HOLDING", flush=True)
    s.settimeout(0.5)
    close_code = None
    deadline = time.time() + hold
    data = rest
    while time.time() < deadline and close_code is None:
        try:
            chunk = s.recv(4096)
            if not chunk:
                print("PEER_CLOSED_TCP", flush=True)
                break
            data += chunk
        except socket.timeout:
            pass
        # parse server frames (unmasked)
        while len(data) >= 2:
            opcode = data[0] & 0x0F
            ln = data[1] & 0x7F
            off = 2
            if ln == 126:
                if len(data) < 4:
                    break
                ln = struct.unpack(">H", data[2:4])[0]
                off = 4
            elif ln == 127:
                if len(data) < 10:
                    break
                ln = struct.unpack(">Q", data[2:10])[0]
                off = 10
            if len(data) < off + ln:
                break
            payload = data[off:off + ln]
            data = data[off + ln:]
            if opcode == 0x8:
                close_code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 0
                print(f"WS_CLOSE_FRAME code={close_code} reason={payload[2:].decode(errors='replace')}", flush=True)
                break
            print(f"WS_FRAME opcode={opcode} len={ln}", flush=True)
    if close_code is None:
        print(f"NO_CLOSE_FRAME_WITHIN {hold}s", flush=True)
    s.close()
    print("CLIENT_CLOSED", flush=True)


def refused(host, port):
    try:
        s = socket.create_connection((host, port), timeout=3)
        print(f"UNEXPECTED_CONNECTED {s.getsockname()}", flush=True)
        s.close()
        return 1
    except OSError as e:
        print(f"REFUSED {host}:{port} ({e.__class__.__name__}: {e})", flush=True)
        return 0


def occupy(port, seconds, address="0.0.0.0"):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind((address, port))
    srv.listen(1)
    print(f"OCCUPYING {address}:{port}", flush=True)
    time.sleep(seconds)
    srv.close()


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "connect":
        connect(sys.argv[2], int(sys.argv[3]), sys.argv[4], float(sys.argv[5]))
    elif mode == "refused":
        sys.exit(refused(sys.argv[2], int(sys.argv[3])))
    elif mode == "occupy":
        occupy(int(sys.argv[2]), float(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else "0.0.0.0")
