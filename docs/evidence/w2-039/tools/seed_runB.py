#!/usr/bin/env python3
"""w2-039 run B TEST FIXTURE: make the desktop Core's current-month file large, then put it back.

  python seed_runB.py seed      back up build/runtime-cwd/data/sensor_202609.sqlite to
                                sensor_202609.sqlite.w2-039-before.bak, then insert one row per
                                second from 2026-09-01 00:00 (local) up to now - 10 min, with the
                                same generator as the PM script history_bench.py (s1..s20 random
                                0..65535).  Rows are only added; existing rows stay.
  python seed_runB.py restore   copy the .bak back over the data file (removes the seeded rows
                                and anything the app wrote during run B) and delete the .bak.
  python seed_runB.py info      row count / min / max timestamp (read-only).

Refuses to run while TaidaFlowApp.exe is running.  Only this repo's git-ignored build/ file is
touched.  No product code is involved: the app later reads the file through its normal path.
"""
import datetime as dt
import random
import shutil
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]          # taidaflow/
DB = ROOT / "build" / "runtime-cwd" / "data" / "sensor_202609.sqlite"
BAK = DB.with_name(DB.name + ".w2-039-before.bak")


def app_running() -> bool:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq TaidaFlowApp.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "TaidaFlowApp.exe" in out


def info(tag: str) -> None:
    d = sqlite3.connect(f"file:{DB.as_posix()}?mode=ro", uri=True)
    n, lo, hi = d.execute("SELECT COUNT(*), MIN(timestamp), MAX(timestamp) FROM sensor_data").fetchone()
    d.close()
    fmt = lambda t: dt.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S") if t else "-"
    print(f"{tag}: {DB.name} rows={n} min={fmt(lo)} max={fmt(hi)} size={DB.stat().st_size / 1024 / 1024:.0f} MB")


cmd = sys.argv[1] if len(sys.argv) > 1 else "info"
if cmd != "info" and app_running():
    print("TaidaFlowApp is running - refusing"); sys.exit(2)
if cmd == "info":
    info("info")
elif cmd == "seed":
    if BAK.exists():
        print(f"{BAK.name} already exists - restore first"); sys.exit(2)
    if not DB.exists():
        print(f"{DB} missing"); sys.exit(2)
    shutil.copy2(DB, BAK)
    info("before seed")
    start = int(dt.datetime(2026, 9, 1).timestamp())
    end = int(time.time()) - 600
    ins = ("INSERT INTO sensor_data (timestamp, " + ", ".join(f"s{i}" for i in range(1, 21))
           + ") VALUES (" + ",".join("?" * 21) + ")")
    random.seed(39)
    d = sqlite3.connect(DB)
    batch = []
    for ts in range(start, end):
        batch.append((ts, *[random.uniform(0, 65535) for _ in range(20)]))
        if len(batch) == 50_000:
            d.executemany(ins, batch); d.commit(); batch.clear()
    if batch:
        d.executemany(ins, batch); d.commit()
    d.close()
    print(f"seeded {end - start} rows ({dt.datetime.fromtimestamp(start)} .. {dt.datetime.fromtimestamp(end - 1)})")
    info("after seed")
elif cmd == "restore":
    if not BAK.exists():
        print(f"{BAK.name} missing"); sys.exit(2)
    shutil.copy2(BAK, DB)
    BAK.unlink()
    info("restored")
else:
    print(__doc__); sys.exit(1)
