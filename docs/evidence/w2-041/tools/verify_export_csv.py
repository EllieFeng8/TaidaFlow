"""w2-041: independent row-by-row check of an exported History CSV.

Rebuilds the expected CSV straight from the month files with Python's sqlite3 and compares
it line by line (streaming, both sides) with the file written by the Core:

  * UTF-8 BOM, header "序號" + the 17 History titles, every cell quoted, CRLF
  * rows newest first: month files newest first, inside a month ORDER BY timestamp DESC,
    rowid DESC; timestamp in [from, to] and > 0
  * 序號 1..N, time = local "yyyy/MM/dd HH:mm:ss"
  * values = raw * scale exactly like Core::applyHistoryPage (TT x100/65535, PT x1000/65535,
    FM-01 x1, M x100/65535) formatted like JS Number.prototype.toFixed(2) (the History page's
    cellText): exact decimal value, ties away from zero - implemented here with Decimal
    ROUND_HALF_UP on |v| (a different method from the C++ side); NULL -> "—"

Usage: python verify_export_csv.py CSV --data DIR --from SEC --to SEC
       (CSV may be a glob pattern; the newest matching file is checked)
Exit code 0 = identical, 1 = mismatch (first 5 differences printed).
"""
import argparse
import datetime as dt
import math
import re
import sqlite3
import sys
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("csv")
ap.add_argument("--data", required=True)
ap.add_argument("--from", dest="frm", type=int, required=True)
ap.add_argument("--to", type=int, required=True)
args = ap.parse_args()

TITLES = ["時間", "TT-01 (°C)", "TT-02 (°C)", "TT-03 (°C)", "TT-04 (°C)",
          "PT-01 (bar)", "PT-02 (bar)", "PT-03 (bar)", "PT-04 (bar)", "PT-05 (bar)",
          "PT-06 (bar)", "PT-07 (bar)", "FM-01 (L/min)", "M1 (%)", "M2 (%)", "M3 (%)", "M4 (%)"]
TT = 100.0 / 65535.0
PT = 1000.0 / 65535.0
SCALES = [TT] * 4 + [PT] * 7 + [1.0] + [TT] * 4
CENT = Decimal("0.01")


def js_to_fixed2(v):
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "Infinity" if v > 0 else "-Infinity"
    if v == 0:
        return "0.00"
    d = Decimal(abs(v)).quantize(CENT, rounding=ROUND_HALF_UP)   # Decimal(float) is exact
    return ("-" if v < 0 else "") + f"{d:.2f}"


def q(cell):
    return '"' + str(cell).replace('"', '""') + '"'


def expected_lines():
    yield "﻿" + ",".join(q(c) for c in ["序號"] + TITLES)
    files = sorted((p for p in Path(args.data).glob("sensor_*.sqlite")
                    if re.fullmatch(r"sensor_\d{6}\.sqlite", p.name)), reverse=True)
    lo = max(args.frm, 1)
    cols = "rowid, timestamp, " + ", ".join(f"s{i}" for i in range(1, 17))
    seq = 0
    for path in files:
        db = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
        cur = db.execute(f"SELECT {cols} FROM sensor_data WHERE timestamp >= ? AND timestamp <= ? "
                         "ORDER BY timestamp DESC, rowid DESC", (lo, args.to))
        for row in cur:
            seq += 1
            cells = [str(seq), dt.datetime.fromtimestamp(row[1]).strftime("%Y/%m/%d %H:%M:%S")]
            for i, raw in enumerate(row[2:]):
                cells.append("—" if raw is None else js_to_fixed2(raw * SCALES[i]))
            yield ",".join(q(c) for c in cells)
        db.close()


def actual_lines(path):
    with open(path, "rb") as f:
        data_iter = iter(f)
        for raw in data_iter:
            if not raw.endswith(b"\r\n"):
                yield None, raw
                continue
            yield raw[:-2].decode("utf-8"), raw


csv_path = args.csv
if any(ch in csv_path for ch in "*?"):
    import glob
    import os
    matches = sorted(glob.glob(csv_path), key=os.path.getmtime)
    if not matches:
        print("no file matches", csv_path)
        sys.exit(2)
    csv_path = matches[-1]
print("checking", csv_path, "size", Path(csv_path).stat().st_size, "bytes")

mismatch = 0
n = 0
exp_iter = expected_lines()
act_iter = actual_lines(csv_path)
shown = 0
while True:
    e = next(exp_iter, None)
    a = next(act_iter, None)
    if e is None and a is None:
        break
    n += 1
    a_text = None if a is None else a[0]
    if e != a_text:
        mismatch += 1
        if shown < 5:
            shown += 1
            print(f"line {n}: expected {e!r}\n        actual   {a_text!r} (raw {None if a is None else a[1][:120]!r})")
rows = n - 1
print(f"lines compared: {n} (header + {rows} rows), mismatches: {mismatch}")
print("RESULT:", "IDENTICAL" if mismatch == 0 else "DIFFERENT")
sys.exit(0 if mismatch == 0 else 1)
