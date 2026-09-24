#!/usr/bin/env python3
"""w2-041: sample the memory of a running process from outside (read-only).

  python mem_sample.py --name TaidaFlowApp.exe --seconds 60 --out mem.csv [--interval 200]

Every --interval ms writes: local time, private bytes (PrivateUsage), working set, peak working
set (MB).  Prints min/max of private bytes and working set at the end.  Uses only
OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) + GetProcessMemoryInfo; nothing is changed.
Exit 2 if the process is not found.
"""
import argparse
import ctypes
import datetime as dt
import subprocess
import sys
import time
from ctypes import wintypes

ap = argparse.ArgumentParser()
ap.add_argument("--name", default="TaidaFlowApp.exe")
ap.add_argument("--seconds", type=float, default=60)
ap.add_argument("--interval", type=float, default=200)
ap.add_argument("--out", required=True)
a = ap.parse_args()

out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {a.name}", "/FO", "CSV", "/NH"],
                     capture_output=True, text=True).stdout.strip().splitlines()
pids = [int(l.split('","')[1]) for l in out if l.startswith('"')]
if not pids:
    print(f"{a.name} not running")
    sys.exit(2)
pid = pids[0]


class PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
h = k32.OpenProcess(0x1000, False, pid)          # PROCESS_QUERY_LIMITED_INFORMATION
if not h:
    print("OpenProcess failed")
    sys.exit(2)
rows = []
end = time.perf_counter() + a.seconds
with open(a.out, "w", encoding="utf-8") as f:
    f.write("time,private_mb,working_set_mb,peak_working_set_mb\n")
    while time.perf_counter() < end:
        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        if not psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
            break                                   # process exited
        r = (dt.datetime.now().strftime("%H:%M:%S.%f")[:-3], pmc.PrivateUsage / 1048576,
             pmc.WorkingSetSize / 1048576, pmc.PeakWorkingSetSize / 1048576)
        rows.append(r)
        f.write(f"{r[0]},{r[1]:.1f},{r[2]:.1f},{r[3]:.1f}\n")
        f.flush()
        time.sleep(a.interval / 1000)
k32.CloseHandle(h)
if rows:
    print(f"pid {pid}: {len(rows)} samples; private {min(r[1] for r in rows):.1f}..{max(r[1] for r in rows):.1f} MB; "
          f"working set {min(r[2] for r in rows):.1f}..{max(r[2] for r in rows):.1f} MB; "
          f"peak working set {rows[-1][3]:.1f} MB")
