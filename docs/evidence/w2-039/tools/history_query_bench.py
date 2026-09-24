"""w2-039 benchmark: old vs new History-page query on a 30-day sensor_data table.

Schema and data generation are the same as the PM script history_bench.py
(timestamp INTEGER + s1..s40 REAL, index idx_sensor_data_ts on timestamp, 1 row per
second, 20 non-null readings per row, 2,592,000 rows = 30 days).  The DB is written to
build/w2-039-bench/data/sensor_202609.sqlite (git-ignored) so that the C++ harness
(tools/qtest) can open the same file through the real SqlManager.

Old load (Core::loadHistoryRecords before w2-039, page 1 = newest):
    countSensorRange COUNT + queryRangeJsonPaged COUNT + ASC LIMIT 10 OFFSET total-10
New load (w2-039, SqlManager::querySensorHistoryPage):
    one COUNT + DESC page, OFFSET (page-1)*10.  Two SQL forms are measured:
      plain : SELECT cols ... ORDER BY timestamp DESC, rowid DESC LIMIT 10 OFFSET ?
      keyed : SELECT cols ... WHERE rowid IN (SELECT rowid ... ORDER BY timestamp DESC,
              rowid DESC LIMIT 10 OFFSET ?) ORDER BY timestamp DESC, rowid DESC
              (deferred join: the OFFSET skip walks only the index; measured for
              comparison only - SqlManager uses the plain form)

Usage: python history_query_bench.py [--db PATH] [--rebuild]
"""
import argparse
import datetime as dt
import os
import random
import sqlite3
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]          # taidaflow/
DEFAULT_DB = ROOT / "build" / "w2-039-bench" / "data" / "sensor_202609.sqlite"
ROWS = 2_592_000
PAGE = 10

ap = argparse.ArgumentParser()
ap.add_argument("--db", default=str(DEFAULT_DB))
ap.add_argument("--rebuild", action="store_true")
args = ap.parse_args()
path = Path(args.db)
path.parent.mkdir(parents=True, exist_ok=True)

month_start = int(dt.datetime(2026, 9, 1, 0, 0, 0).timestamp())      # local time, like Core
month_end = int(dt.datetime(2026, 10, 1, 0, 0, 0).timestamp()) - 1
sel_cols = "timestamp, " + ", ".join(f"s{i}" for i in range(1, 41))


def build():
    if path.exists():
        path.unlink()
    db = sqlite3.connect(path)
    db.execute("PRAGMA journal_mode=DELETE")
    cols = ", ".join(f"s{i} REAL" for i in range(1, 41))
    db.execute(f"CREATE TABLE sensor_data (timestamp INTEGER NOT NULL, {cols})")
    db.execute("CREATE INDEX idx_sensor_data_ts ON sensor_data(timestamp)")
    db.commit()
    ins = ("INSERT INTO sensor_data (timestamp, " + ", ".join(f"s{i}" for i in range(1, 21))
           + ") VALUES (" + ",".join("?" * 21) + ")")
    random.seed(39)
    batch, ts = [], month_start
    for _ in range(ROWS):
        batch.append((ts, *[random.uniform(0, 65535) for _ in range(20)]))
        ts += 1
        if len(batch) == 50_000:
            db.executemany(ins, batch); db.commit(); batch.clear()
    if batch:
        db.executemany(ins, batch); db.commit()
    db.close()


if args.rebuild or not path.exists():
    t = time.perf_counter(); build()
    print(f"built {path} in {time.perf_counter() - t:.0f} s")

db = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
total = db.execute("SELECT COUNT(1) FROM sensor_data WHERE timestamp >= ? AND timestamp <= ?",
                   (month_start, month_end)).fetchone()[0]
print(f"db={path} rows={total} size={os.path.getsize(path) / 1024 / 1024:.0f} MB")
last_page = (total + PAGE - 1) // PAGE


def timed(sql, params, reps=5):
    best, first, rows = None, None, None
    for i in range(reps):
        t = time.perf_counter(); rows = db.execute(sql, params).fetchall()
        ms = (time.perf_counter() - t) * 1000
        first = ms if i == 0 else first
        best = ms if best is None else min(best, ms)
    return best, first, rows


count_sql = "SELECT COUNT(1) FROM sensor_data WHERE timestamp >= ? AND timestamp <= ?"
asc_sql = (f"SELECT {sel_cols} FROM sensor_data WHERE timestamp >= ? AND timestamp <= ? "
           "ORDER BY timestamp LIMIT ? OFFSET ?")
plain_sql = (f"SELECT {sel_cols} FROM sensor_data WHERE timestamp >= ? AND timestamp <= ? "
             "ORDER BY timestamp DESC, rowid DESC LIMIT ? OFFSET ?")
keyed_sql = (f"SELECT {sel_cols} FROM sensor_data WHERE rowid IN ("
             "SELECT rowid FROM sensor_data WHERE timestamp >= ? AND timestamp <= ? "
             "ORDER BY timestamp DESC, rowid DESC LIMIT ? OFFSET ?) "
             "ORDER BY timestamp DESC, rowid DESC")

c_ms, c_first, _ = timed(count_sql, (month_start, month_end))
print(f"\nCOUNT(1) month range: best {c_ms:.1f} ms (first {c_first:.1f} ms)")

print(f"\n{'variant':<34} {'page':>7} {'page ms':>9} {'load ms (best)':>15}  newest ts of page")
o_ms, _, rows = timed(asc_sql, (month_start, month_end, PAGE, max(0, total - PAGE)))
print(f"{'OLD 2xCOUNT + ASC OFFSET total-10':<34} {1:>7} {o_ms:>9.1f} {2 * c_ms + o_ms:>15.1f}  {rows[-1][0]}")
for p in (1, 1000, 10_000, last_page // 2, last_page):
    off = (p - 1) * PAGE
    pm, _, prow = timed(plain_sql, (month_start, month_end, PAGE, off))
    km, _, krow = timed(keyed_sql, (month_start, month_end, PAGE, off))
    assert [r[0] for r in prow] == [r[0] for r in krow], "plain/keyed differ"
    print(f"{'NEW COUNT + DESC plain (used)':<34} {p:>7} {pm:>9.1f} {c_ms + pm:>15.1f}  {prow[0][0]}")
    print(f"{'NEW COUNT + DESC keyed':<34} {p:>7} {km:>9.1f} {c_ms + km:>15.1f}  {krow[0][0]}")

# Order check: page 1 newest-first equals the old ASC last page reversed.
_, _, asc_rows = timed(asc_sql, (month_start, month_end, PAGE, total - PAGE), reps=1)
_, _, new_rows = timed(plain_sql, (month_start, month_end, PAGE, 0), reps=1)
print("\npage1 == reversed(old ASC last 10):", [r[0] for r in new_rows] == [r[0] for r in reversed(asc_rows)])
for name, sql in (("plain (used)", plain_sql), ("keyed", keyed_sql)):
    print(f"query plan {name}:")
    for row in db.execute("EXPLAIN QUERY PLAN " + sql, (month_start, month_end, PAGE, 0)):
        print("   ", row)
db.close()
