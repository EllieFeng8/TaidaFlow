#!/usr/bin/env python3
"""w2-039: click one point of the TaidaFlow window N times, --gap ms apart (fast paging test).

  python fast_clicks.py X Y --count 3 --gap 30 --expect-left 949 --expect-top 459

X/Y are window-relative (same frame as scripts/desktop_input.py).  Refuses (exit 4) when the
window is not at the expected position.  Appends one line to docs/evidence/w2-039/input-log.txt.
"""
import argparse
import ctypes
import datetime as dt
import sys
import time
from ctypes import wintypes
from pathlib import Path

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()
ULONG_PTR = ctypes.c_size_t


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def mouse(flags):
    i = INPUT(type=0)
    i.mi = MOUSEINPUT(0, 0, 0, flags, 0, 0)
    user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


ap = argparse.ArgumentParser()
ap.add_argument("x", type=int)
ap.add_argument("y", type=int)
ap.add_argument("--count", type=int, default=3)
ap.add_argument("--gap", type=float, default=30)
ap.add_argument("--expect-left", type=int, required=True)
ap.add_argument("--expect-top", type=int, required=True)
ap.add_argument("--note", default="")
a = ap.parse_args()

log = Path(__file__).resolve().parents[1] / "input-log.txt"
hwnd = user32.FindWindowW(None, "TaidaFlow")
rect = wintypes.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(rect))
stamp = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
if not hwnd or rect.left != a.expect_left or rect.top != a.expect_top:
    line = f"[{stamp}] w2-039 fast clicks REFUSED: window at L={rect.left} T={rect.top} : {a.note}"
    log.open("a", encoding="utf-8").write(line + "\n"); print(line); sys.exit(4)
user32.SetCursorPos(rect.left + a.x, rect.top + a.y)
time.sleep(0.15)
times = []
for _ in range(a.count):
    times.append(dt.datetime.now().strftime("%H:%M:%S.%f")[:-3])
    mouse(0x2); time.sleep(0.005); mouse(0x4)
    time.sleep(a.gap / 1000)
line = (f"[{stamp}] w2-039 fast clicks x{a.count} at window({a.x},{a.y}) gap {a.gap:.0f} ms, "
        f"click times {', '.join(times)} : {a.note}")
log.open("a", encoding="utf-8").write(line + "\n")
print(line)
