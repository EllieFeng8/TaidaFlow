#!/usr/bin/env python3
"""(copied unchanged from docs/evidence/w2-039/tools/ui_ping.py for w2-041) w2-039: measure how quickly the TaidaFlowApp GUI (main) thread answers, from outside.

Every --interval ms a WM_NULL is sent to the "TaidaFlow" top-level window with
SendMessageTimeoutW.  The call returns only when the window's thread (the Qt main
thread) pumps messages, so its round-trip time is the main-thread stall at that moment.
Nothing is changed in the app; WM_NULL does nothing.

  python ui_ping.py --seconds 120 --out ping.csv [--interval 10]

Writes one CSV line per ping (local time HH:MM:SS.fff, latency ms) and prints a summary:
count, p50/p99/max latency and every ping above --report-ms (default 20 ms).
Exit 0 on success, 2 if the window is not found.
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
SendMessageTimeoutW = user32.SendMessageTimeoutW
SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
                                wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
SendMessageTimeoutW.restype = ctypes.c_size_t
WM_NULL, SMTO_NORMAL = 0x0000, 0x0000

ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=float, default=60)
ap.add_argument("--interval", type=float, default=10, help="ms between pings")
ap.add_argument("--out", required=True)
ap.add_argument("--report-ms", type=float, default=20)
ap.add_argument("--title", default="TaidaFlow")
a = ap.parse_args()

hwnd = user32.FindWindowW(None, a.title)
if not hwnd:
    print(f"{a.title} window not found")
    sys.exit(2)

samples = []
end = time.perf_counter() + a.seconds
result = ctypes.c_size_t()
with open(a.out, "w", encoding="utf-8") as f:
    f.write("time,latency_ms\n")
    while time.perf_counter() < end:
        stamp = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        t = time.perf_counter()
        ok = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 5000, ctypes.byref(result))
        ms = (time.perf_counter() - t) * 1000
        if not ok:
            ms = float("inf")
        samples.append((stamp, ms))
        f.write(f"{stamp},{ms:.2f}\n")
        time.sleep(max(0.0, a.interval / 1000 - (time.perf_counter() - t)))

lat = sorted(s[1] for s in samples)
p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))]
print(f"pings={len(lat)} p50={p(0.5):.2f} ms p99={p(0.99):.2f} ms max={lat[-1]:.2f} ms "
      f"(window {a.title!r} hwnd={hwnd:#x}, {a.seconds:.0f} s, every {a.interval:.0f} ms)")
slow = [s for s in samples if s[1] > a.report_ms]
print(f"pings above {a.report_ms:.0f} ms: {len(slow)}")
for stamp, ms in slow:
    print(f"  {stamp} {ms:.1f} ms")
