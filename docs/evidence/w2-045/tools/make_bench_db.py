"""w2-045 bench data: 12 month files of one-row-per-second sensor data.

Same schema and style as w2-041 make_bench_db.py (sensor_data(timestamp INTEGER NOT NULL,
s1..s40 REAL), index idx_sensor_data_ts created BEFORE the rows are inserted so that index
and table pages interleave in the file as they do in the application, rollback journal
(journal_mode=DELETE), s1..s20 random in 0..65535, s21..s40 NULL).

Written to taidaflow/build/w2-045-bench/data (git-ignored, about 7 GB):

  sensor_202510.sqlite .. sensor_202608.sqlite   every second of the whole month
  sensor_202609.sqlite                           2026-09-01 00:00:00 .. 2026-09-24 23:59:59
  (31,017,600 rows + 5 duplicates per month)

Extras (deterministic, seed 45): 5 extra rows per month that repeat existing timestamps
(inserted last, so rowid breaks the tie), incl. the first and the last second of the month.

Usage: python -B make_bench_db.py [--rebuild] [--months N]
Checks free disk space first (needs 10 GB) and prints the file list with row counts.
"""
import argparse
import datetime as dt
import random
import shutil
import sqlite3
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]          # taidaflow/
DATA = ROOT / "build" / "w2-045-bench" / "data"

ap = argparse.ArgumentParser()
ap.add_argument("--rebuild", action="store_true")
ap.add_argument("--months", type=int, default=12)
args = ap.parse_args()


def ts(d: dt.datetime) -> int:
    return int(d.timestamp())                           # local time, like Core


LAST_MONTH = dt.datetime(2026, 9, 1)
END = dt.datetime(2026, 9, 25)                          # exclusive


def month_starts(n):
    y, m = LAST_MONTH.year, LAST_MONTH.month
    out = []
    for _ in range(n):
        out.append(dt.datetime(y, m, 1))
        m -= 1
        if m == 0:
            y, m = y - 1, 12
    return sorted(out)


def next_month(d):
    return dt.datetime(d.year + (d.month == 12), d.month % 12 + 1, 1)


MONTHS = month_starts(args.months)
ins = ("INSERT INTO sensor_data (timestamp, " + ", ".join(f"s{i}" for i in range(1, 21))
       + ") VALUES (" + ",".join("?" * 21) + ")")


def create(path):
    if path.exists():
        path.unlink()
    db = sqlite3.connect(path)
    db.execute("PRAGMA journal_mode=DELETE")
    db.execute("PRAGMA synchronous=OFF")                # generation only (not stored in the file)
    cols = ", ".join(f"s{i} REAL" for i in range(1, 41))
    db.execute(f"CREATE TABLE sensor_data (timestamp INTEGER NOT NULL, {cols})")
    db.execute("CREATE INDEX idx_sensor_data_ts ON sensor_data(timestamp)")
    hcols = ", ".join(f"h{i} INTEGER" for i in range(1, 101))
    db.execute(f"CREATE TABLE holding_register (timestamp INTEGER NOT NULL, {hcols})")
    db.execute("CREATE INDEX idx_holding_register_ts ON holding_register(timestamp)")
    db.execute("CREATE TABLE alarm_history (id INTEGER PRIMARY KEY, occurrence_time INTEGER, reason VARCHAR(255))")
    db.execute("CREATE INDEX idx_alarm_history_time ON alarm_history(occurrence_time)")
    db.commit()
    return db


def build_month(start, pool, rng):
    key = start.strftime("%Y%m")
    path = DATA / f"sensor_{key}.sqlite"
    first = ts(start)
    last = ts(min(next_month(start), END)) - 1
    db = create(path)
    batch = []
    k = 0
    for t in range(first, last + 1):
        batch.append((t, *pool[k % len(pool)]))
        k += 1
        if len(batch) == 100_000:
            db.executemany(ins, batch)
            db.commit()
            batch.clear()
    for t in (first, last, first + 86400 * 3 + 7, first + (last - first) // 2, last - 3600):
        batch.append((t, *[rng.uniform(0, 65535) for _ in range(20)]))
    db.executemany(ins, batch)
    db.commit()
    db.close()
    return path


def main():
    DATA.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(DATA).free
    print(f"free disk space on {DATA.drive or DATA}: {free / 2**30:.1f} GiB")
    if free < 10 * 2**30:
        print("need at least 10 GiB free - not building")
        return 2
    rng = random.Random(45)
    pool = [tuple(rng.uniform(0, 65535) for _ in range(20)) for _ in range(10007)]
    for start in MONTHS:
        path = DATA / f"sensor_{start.strftime('%Y%m')}.sqlite"
        if path.exists() and not args.rebuild:
            continue
        t = time.perf_counter()
        build_month(start, pool, rng)
        print(f"built {path.name} in {time.perf_counter() - t:.0f} s", flush=True)
    total = 0
    for start in MONTHS:
        path = DATA / f"sensor_{start.strftime('%Y%m')}.sqlite"
        db = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
        c, lo, hi, mx = db.execute("SELECT COUNT(1), MIN(timestamp), MAX(timestamp), MAX(rowid) FROM sensor_data").fetchone()
        jm = db.execute("PRAGMA journal_mode").fetchone()[0]
        db.close()
        total += c
        print(f"{path.name}: rows={c} min={lo} max={hi} max_rowid={mx} size={path.stat().st_size} journal={jm}")
    print(f"total rows={total}; range {ts(MONTHS[0])} .. {ts(END) - 1} s")
    print(f"free disk space after: {shutil.disk_usage(DATA).free / 2**30:.1f} GiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
