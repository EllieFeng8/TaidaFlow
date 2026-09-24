#!/usr/bin/env python3
"""E2E helper: drive the real TaidaFlow desktop UI with OS mouse/keyboard input (Windows).

Coordinates are relative to the top-left of the TaidaFlowApp window rectangle, i.e. the
same frame as screenshots taken with scripts/capture-window.ps1.

  python scripts/desktop_input.py info
  python scripts/desktop_input.py click X Y
  python scripts/desktop_input.py type "12.5"          (plain ASCII text, via Unicode SendInput)
  python scripts/desktop_input.py key enter|esc|tab|backspace|ctrl+a
  python scripts/desktop_input.py close                (WM_CLOSE, like clicking the X button)
  python scripts/desktop_input.py --title "下載歷史資料" click X Y   (target another top-level
                                                       window by exact title, e.g. a native dialog)

Exit code 0 on success, 2 if the window is not found.
"""
from __future__ import annotations

import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()

INPUT_MOUSE, INPUT_KEYBOARD = 0, 1
KEYEVENTF_KEYUP, KEYEVENTF_UNICODE = 0x2, 0x4
MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x2, 0x4
ULONG_PTR = ctypes.c_size_t


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD)]


class _U(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def send(*inputs: INPUT) -> None:
    arr = (INPUT * len(inputs))(*inputs)
    user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))


def key_input(vk: int = 0, scan: int = 0, flags: int = 0) -> INPUT:
    i = INPUT(type=INPUT_KEYBOARD)
    i.ki = KEYBDINPUT(vk, scan, flags, 0, 0)
    return i


def mouse_input(flags: int) -> INPUT:
    i = INPUT(type=INPUT_MOUSE)
    i.mi = MOUSEINPUT(0, 0, 0, flags, 0, 0)
    return i


def find_window(title: str = "TaidaFlow") -> int:
    hwnd = user32.FindWindowW(None, title)
    return hwnd


def focus(hwnd: int) -> None:
    if user32.GetForegroundWindow() == hwnd:
        return  # already active: no ALT tap (it would move focus inside dialogs)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    send(key_input(0x12), key_input(0x12, flags=KEYEVENTF_KEYUP))  # ALT tap unlocks foreground
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.25)


VK = {"enter": 0x0D, "esc": 0x1B, "tab": 0x09, "backspace": 0x08, "ctrl": 0x11, "a": 0x41}


def main(argv: list[str]) -> int:
    title = "TaidaFlow"
    if len(argv) >= 3 and argv[1] == "--title":
        # e.g. the desktop CSV export's native save dialog: --title 下載歷史資料
        title = argv[2]
        argv = argv[:1] + argv[3:]
    if len(argv) < 2:
        print(__doc__)
        return 1
    hwnd = find_window(title)
    if not hwnd:
        print(f"{title} window not found")
        return 2
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    action = argv[1]
    if action == "info":
        print(f"window L={rect.left} T={rect.top} R={rect.right} B={rect.bottom} "
              f"screen={user32.GetSystemMetrics(0)}x{user32.GetSystemMetrics(1)}")
        return 0
    if action == "close":
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE: normal window close
        print("WM_CLOSE posted")
        return 0
    focus(hwnd)
    if action == "click":
        x, y = int(argv[2]), int(argv[3])
        sx, sy = rect.left + x, rect.top + y
        user32.SetCursorPos(sx, sy)
        time.sleep(0.12)
        send(mouse_input(MOUSEEVENTF_LEFTDOWN))
        time.sleep(0.06)
        send(mouse_input(MOUSEEVENTF_LEFTUP))
        print(f"clicked window({x},{y}) screen({sx},{sy})")
    elif action == "type":
        for ch in argv[2]:
            send(key_input(scan=ord(ch), flags=KEYEVENTF_UNICODE),
                 key_input(scan=ord(ch), flags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP))
            time.sleep(0.03)
        print(f"typed {argv[2]!r}")
    elif action == "key":
        keys = [VK[k] for k in argv[2].lower().split("+")]
        send(*[key_input(k) for k in keys])
        send(*[key_input(k, flags=KEYEVENTF_KEYUP) for k in reversed(keys)])
        print(f"key {argv[2]}")
    else:
        print(f"unknown action {action}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
