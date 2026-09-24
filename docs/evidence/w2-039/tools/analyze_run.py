#!/usr/bin/env python3
"""w2-039: summarise a desktop run log (QT_MESSAGE_PATTERN '%{time HH:mm:ss.zzz} [t<id>] ...',
UTF-8 copy) and, optionally, the ui_ping.py CSV of the same run.

  python analyze_run.py <desktop-log> [--ping ping.csv]

Prints:
  * number of per-second saves ('[SQL] Saved Server Input Registers') and of History requests,
    with the reason of every request; how many saves happened between consecutive requests
    (proves that saving no longer triggers a load);
  * every [History] line (request / applied / dropped);
  * with --ping: the largest main-thread ping latency in each window [request, apply + 50 ms],
    and the overall p50/p99/max outside those windows.
"""
import argparse
import csv
import datetime as dt
import re

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--ping")
a = ap.parse_args()


def t(s):
    return dt.datetime.strptime(s, "%H:%M:%S.%f")


saves, events = [], []
with open(a.log, encoding="utf-8", errors="replace") as f:
    for line in f:
        m = re.match(r"(\d\d:\d\d:\d\d\.\d{3}) \[t\d+\] (.*)", line.rstrip("\n"))
        if not m:
            continue
        ts, msg = t(m.group(1)), m.group(2)
        if msg.startswith("[SQL] Saved Server Input Registers"):
            saves.append(ts)
        elif msg.startswith("[History]"):
            events.append((ts, msg))

requests = [(ts, msg) for ts, msg in events if " request #" in msg]
print(f"saves: {len(saves)} ({saves[0]:%H:%M:%S} .. {saves[-1]:%H:%M:%S})" if saves else "saves: 0")
print(f"History requests: {len(requests)}")
for ts, msg in requests:
    print(f"  {ts:%H:%M:%S.%f}"[:-3] + f"  {msg}")
bounds = [dt.datetime.min] + [ts for ts, _ in requests] + [dt.datetime.max]
print("saves between consecutive History requests (none of them caused a load):")
for i in range(len(bounds) - 1):
    n = sum(1 for s in saves if bounds[i] <= s < bounds[i + 1])
    lo = "start" if i == 0 else f"request #{i}"
    hi = "end" if i == len(bounds) - 2 else f"request #{i + 1}"
    print(f"  {lo:>12} -> {hi:<12}: {n} saves")
print("all [History] lines:")
for ts, msg in events:
    print(f"  {ts:%H:%M:%S.%f}"[:-3] + f"  {msg}")

if a.ping:
    pings = []
    with open(a.ping, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            pings.append((t(row["time"]), float(row["latency_ms"])))
    print(f"\nmain-thread ping ({len(pings)} pings, {pings[0][0]:%H:%M:%S} .. {pings[-1][0]:%H:%M:%S}):")
    windows = []
    for ts, msg in events:
        m = re.search(r"request #(\d+)", msg)
        if m:
            rid = m.group(1)
            done = [e for e, mm in events if re.search(rf"(#{rid} applied|result #{rid} )", mm)]
            end = (done[0] if done else ts) + dt.timedelta(milliseconds=50)
            windows.append((rid, ts, end))
    inside = set()
    for rid, lo, hi in windows:
        sel = [(p, ms) for p, ms in pings if lo <= p <= hi]
        inside.update(p for p, _ in sel)
        if sel:
            worst = max(sel, key=lambda x: x[1])
            print(f"  request #{rid}: {len(sel)} pings during load window "
                  f"{lo:%H:%M:%S.%f}"[:-3] + f"..{hi:%H:%M:%S.%f}"[:-3]
                  + f", max {worst[1]:.2f} ms at {worst[0]:%H:%M:%S.%f}"[:-3])
        else:
            print(f"  request #{rid}: no ping in window (outside the ping run)")
    rest = sorted(ms for p, ms in pings if p not in inside)
    q = lambda x: rest[min(len(rest) - 1, int(x * len(rest)))]
    print(f"  outside load windows: p50 {q(0.5):.2f} ms, p99 {q(0.99):.2f} ms, max {rest[-1]:.2f} ms")
