"""w2-041 test data: 30 days of sensor rows spread over two month files.

Same schema and generator style as w2-039 (PM history_bench.py): sensor_data(timestamp
INTEGER NOT NULL, s1..s40 REAL), index idx_sensor_data_ts, rollback journal, one row per
second, s1..s20 = random.uniform(0, 65535).  Written to build/w2-041-bench/data (git-ignored):

  sensor_202608.sqlite  2026-08-17 00:00:00 .. 2026-08-31 23:59:59   1,296,000 rows
  sensor_202609.sqlite  2026-09-01 00:00:00 .. 2026-09-15 23:59:59   1,296,000 rows + 3
  sensor_202607.sqlite  2026-07-10 12:00:00 .. +99 s                  100 rows (outside the
                        30-day range; only seen by the "all months" range)

Extra content to exercise the export/paging rules (all deterministic, seed 41):
  * every 997th row has s3 = NULL and every 1009th row s14 = NULL -> CSV cell "—"
  * s12 (FM-01, scale 1) holds exact binary ties on every 101st row (x.125/.375/.625/.875)
    -> JS toFixed(2) rounds them up
  * 3 extra rows in September repeat an existing timestamp (rowid breaks the tie)
  * 2 rows with timestamp 0 / -5 in the July file (never shown or exported)

Usage: python make_bench_db.py [--rebuild]
Prints the file list and row counts; ~1-2 minutes when building.
"""
import argparse
import datetime as dt
import random
import sqlite3
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]          # taidaflow/
DATA = ROOT / "build" / "w2-041-bench" / "data"

ap = argparse.ArgumentParser()
ap.add_argument("--rebuild", action="store_true")
args = ap.parse_args()
DATA.mkdir(parents=True, exist_ok=True)


def ts(y, m, d, hh=0, mm=0, ss=0):
    return int(dt.datetime(y, m, d, hh, mm, ss).timestamp())    # local time, like Core


FILES = {
    "202607": [(ts(2026, 7, 10, 12), 100)],
    "202608": [(ts(2026, 8, 17), 15 * 86400)],
    "202609": [(ts(2026, 9, 1), 15 * 86400)],
}
TIES = [12.125, 0.375, 7.625, 3.875, 100.125, 0.125]


def create(path):
    if path.exists():
        path.unlink()
    db = sqlite3.connect(path)
    db.execute("PRAGMA journal_mode=DELETE")
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


ins = ("INSERT INTO sensor_data (timestamp, " + ", ".join(f"s{i}" for i in range(1, 21))
       + ") VALUES (" + ",".join("?" * 21) + ")")


def build():
    random.seed(41)
    n = 0
    for key, spans in FILES.items():
        path = DATA / f"sensor_{key}.sqlite"
        db = create(path)
        batch = []
        for start, count in spans:
            for k in range(count):
                vals = [random.uniform(0, 65535) for _ in range(20)]
                if n % 997 == 0:
                    vals[2] = None
                if n % 1009 == 0:
                    vals[13] = None
                if n % 101 == 0:
                    vals[11] = TIES[(n // 101) % len(TIES)]
                batch.append((start + k, *vals))
                n += 1
                if len(batch) == 50_000:
                    db.executemany(ins, batch); db.commit(); batch.clear()
        if key == "202609":
            for extra in (ts(2026, 9, 1, 0, 0, 5), ts(2026, 9, 8, 12), ts(2026, 9, 15, 23, 59, 59)):
                batch.append((extra, *[random.uniform(0, 65535) for _ in range(20)]))
        if key == "202607":
            batch.append((0, *[1.0] * 20))
            batch.append((-5, *[2.0] * 20))
        db.executemany(ins, batch); db.commit()
        db.close()


if args.rebuild or not all((DATA / f"sensor_{k}.sqlite").exists() for k in FILES):
    t = time.perf_counter()
    build()
    print(f"built in {time.perf_counter() - t:.0f} s")

for key in FILES:
    path = DATA / f"sensor_{key}.sqlite"
    db = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
    c, lo, hi = db.execute("SELECT COUNT(1), MIN(timestamp), MAX(timestamp) FROM sensor_data").fetchone()
    print(f"{path.name}: rows={c} min={lo} max={hi} size={path.stat().st_size} "
          f"journal={db.execute('PRAGMA journal_mode').fetchone()[0]}")
    db.close()
print(f"30-day range: {ts(2026, 8, 17)} .. {ts(2026, 9, 16) - 1} s "
      f"= {ts(2026, 8, 17) * 1000} .. {ts(2026, 9, 16) * 1000 - 1} ms")
