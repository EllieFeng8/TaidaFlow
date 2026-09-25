"""w2-045: EXPLAIN QUERY PLAN + one timing for every SQL statement the stepped History request
uses (same text as Core/SqlManager.cpp), on one month file of the 12-month bench, read-only.

  python -B docs/evidence/w2-045/tools/query_plans.py [month file]

Expected: COUNT / chunk-boundary statements use the COVERING timestamp index, page reads use
the timestamp index (no temp B-tree for ORDER BY), the rowid-delta statements use the rowid
range (INTEGER PRIMARY KEY).  Bound values as SqlManager binds them (a cursor/anchor
row also narrows :hi (DESC) or :lo (ASC) to its timestamp).  Note: Python's sqlite3 (not Qt's bundled SQLite) plans them.
"""
import sqlite3
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "w2-045-bench" / "data" / "sensor_202603.sqlite"
db = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
lo, hi, mx = db.execute("SELECT MIN(timestamp), MAX(timestamp), MAX(rowid) FROM sensor_data").fetchone()
mid = (lo + hi) // 2
cols = "rowid, timestamp, " + ", ".join(f"s{i}" for i in range(1, 41))
W = "timestamp >= :lo AND timestamp <= :hi"
stmts = [
    ("snapshot MAX(rowid)", "SELECT MAX(rowid) FROM sensor_data", {}),
    ("chunk boundary DESC (first)", f"SELECT timestamp, rowid FROM sensor_data WHERE {W} ORDER BY timestamp DESC, rowid DESC LIMIT 1 OFFSET :k",
     {"k": 99999}),
    ("chunk boundary DESC (cursor, snapshot)", f"SELECT timestamp, rowid FROM sensor_data WHERE {W} AND +rowid <= :snap AND (timestamp < :cts OR +rowid < :crid) ORDER BY timestamp DESC, rowid DESC LIMIT 1 OFFSET :k",
     {"k": 99999, "snap": mx, "cts": mid, "crid": 10**9, "hi": mid}),
    ("chunk boundary ASC (cursor)", f"SELECT timestamp, rowid FROM sensor_data WHERE {W} AND (timestamp > :cts OR +rowid > :crid) ORDER BY timestamp ASC, rowid ASC LIMIT 1 OFFSET :k",
     {"k": 99999, "cts": mid, "crid": 0, "lo": mid}),
    ("chunk remainder COUNT", f"SELECT COUNT(1) FROM sensor_data WHERE {W} AND (timestamp < :cts OR +rowid < :crid)",
     {"cts": lo + 50000, "crid": 10**9, "hi": lo + 50000}),
    ("cache delta COUNT (rowid range)", "SELECT COUNT(1) FROM sensor_data NOT INDEXED WHERE rowid > :r0 AND rowid <= :r1 AND timestamp >= :lo AND timestamp <= :hi",
     {"r0": mx - 1000, "r1": mx}),
    ("anchor delta COUNT (rowid range)", "SELECT COUNT(1), TOTAL(CASE WHEN timestamp > :ats1 OR (timestamp = :ats2 AND rowid > :arid) THEN 1 ELSE 0 END) FROM sensor_data NOT INDEXED WHERE rowid > :r0 AND rowid <= :r1 AND timestamp >= :lo AND timestamp <= :hi",
     {"ats1": mid, "ats2": mid, "arid": 5, "r0": mx - 1000, "r1": mx}),
    ("page OFFSET DESC", f"SELECT {cols} FROM sensor_data WHERE {W} ORDER BY timestamp DESC, rowid DESC LIMIT :limit OFFSET :offset",
     {"limit": 10, "offset": 40000}),
    ("page OFFSET ASC (snapshot)", f"SELECT {cols} FROM sensor_data WHERE {W} AND +rowid <= :snap ORDER BY timestamp ASC, rowid ASC LIMIT :limit OFFSET :offset",
     {"snap": mx, "limit": 10, "offset": 40000}),
    ("page keyset older-or-equal", f"SELECT {cols} FROM sensor_data WHERE {W} AND (timestamp < :ats OR +rowid <= :arid) ORDER BY timestamp DESC, rowid DESC LIMIT :limit OFFSET :offset",
     {"ats": mid, "arid": 10**9, "limit": 10, "offset": 1, "hi": mid}),
    ("page keyset newer", f"SELECT {cols} FROM sensor_data WHERE {W} AND (timestamp > :ats OR +rowid > :arid) ORDER BY timestamp ASC, rowid ASC LIMIT :limit OFFSET :offset",
     {"ats": mid, "arid": 0, "limit": 10, "offset": 0, "lo": mid}),
]
print(f"sqlite {sqlite3.sqlite_version}; {path.name}: rows up to rowid {mx}, timestamps {lo} .. {hi}")
for name, sql, params in stmts:
    p = {"lo": lo, "hi": hi} | params
    p = {k: v for k, v in p.items() if f":{k}" in sql}
    plan = [r[3] for r in db.execute("EXPLAIN QUERY PLAN " + sql, p).fetchall()]
    db.execute(sql, p).fetchall()
    t = time.perf_counter()
    db.execute(sql, p).fetchall()
    ms = (time.perf_counter() - t) * 1000
    print(f"{name:40s} {ms:7.2f} ms  {plan}")
