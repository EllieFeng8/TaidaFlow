#!/usr/bin/env python3
"""TEST FIXTURE (dev/E2E only): put known sensor rows into the desktop Core's own SQLite file.

Why: on this bench no ADAM module is reachable, so Manager::saveServerInputData() never runs
and the History page is empty. To exercise the History page / CSV export with real data the
rows are written *into the Core's database file* while the app is NOT running; the desktop
Core then loads them at start-up through its normal read path
(Core::loadHistoryRecords -> SqlManager::countSensorRange / queryRangeJsonPaged ->
TaidaFlowProxy.historyRecords -> Mirror -> WASM). No product code is touched or compiled.

What is NOT exercised: the Core's own *insert* path (Manager -> SqlManager::saveSensorData);
this script inserts with Python's sqlite3 into the table the Core created (schema checked).

Safety guards:
  * the database must be build/runtime-cwd/data/sensor_<yyyyMM>.sqlite of THIS repo;
  * it must already exist (created by the Core itself on a previous launch: the start-up
    alarm insert creates it), so the schema is the Core's, not ours;
  * refuses to run while TaidaFlowApp.exe is running.

Usage:
  python scripts/seed_history_sqlite.py [--rows 8] [--step-min 5] [--out rows.json]
Writes the inserted raw rows plus the values the History page is expected to show
(same scaling as Core::loadHistoryRecords, formatted like HistoryPage.qml cellText) to --out.
Exit 0 on success.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sqlite3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME_DATA = ROOT / "build" / "runtime-cwd" / "data"
ADC_FULL_SCALE = 65535.0

# Engineering values chosen per row; raw register = round(value * 65535 / span).
# Column layout of Core::loadHistoryRecords: s1..s4 TT (x100/65535), s5..s11 PT
# (x1000/65535), s12 FM (raw), s13..s16 M1..M4 (x100/65535).


def app_running() -> bool:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq TaidaFlowApp.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "TaidaFlowApp.exe" in out


def cell_text(value: float | None) -> str:
    # HistoryPage.qml cellText(): null -> "—", number -> toFixed(2)
    return "—" if value is None else f"{value:.2f}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", type=int, default=8)
    ap.add_argument("--step-min", type=int, default=5)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    if app_running():
        print("TaidaFlowApp.exe is running - close it first", file=sys.stderr)
        return 2
    now = dt.datetime.now().replace(microsecond=0)
    db_path = RUNTIME_DATA / f"sensor_{now:%Y%m}.sqlite"
    if not db_path.is_file():
        print(f"missing {db_path}: launch the desktop app once (scripts/run-desktop.ps1) so the Core "
              "creates its own database, then close it", file=sys.stderr)
        return 2

    con = sqlite3.connect(db_path)
    cols = [r[1] for r in con.execute("PRAGMA table_info(sensor_data)")]
    if cols[:17] != ["timestamp"] + [f"s{i}" for i in range(1, 17)]:
        print(f"unexpected sensor_data schema: {cols[:17]}", file=sys.stderr)
        return 3
    if con.execute("SELECT COUNT(1) FROM sensor_data").fetchone()[0] != 0:
        print("sensor_data is not empty - refusing to mix fixture rows with existing data", file=sys.stderr)
        return 3

    rows = []
    for k in range(args.rows):
        ts = int((now - dt.timedelta(minutes=args.step_min * (args.rows - 1 - k))).timestamp())
        tt = [20.0 + k * 0.5 + j * 2.0 for j in range(4)]            # degC
        pt = [1.0 + k * 0.05 + j * 0.1 for j in range(7)]            # bar
        fm = None if k == 3 else 10.0 + k * 1.25                      # L/min, one NULL cell
        mv = [10.0 * (j + 1) + k for j in range(4)]                   # %
        raw = ([round(v * ADC_FULL_SCALE / 100.0) for v in tt]
               + [round(v * ADC_FULL_SCALE / 1000.0) for v in pt]
               + [fm]
               + [round(v * ADC_FULL_SCALE / 100.0) for v in mv])
        shown = ([r * 100.0 / ADC_FULL_SCALE for r in raw[0:4]]
                 + [r * 1000.0 / ADC_FULL_SCALE for r in raw[4:11]]
                 + [raw[11]]
                 + [r * 100.0 / ADC_FULL_SCALE for r in raw[12:16]])
        con.execute("INSERT INTO sensor_data (timestamp, " + ", ".join(f"s{i}" for i in range(1, 17))
                    + ") VALUES (" + ", ".join("?" * 17) + ")", [ts] + raw)
        rows.append({
            "timestamp": ts,
            "time": dt.datetime.fromtimestamp(ts).strftime("%Y/%m/%d %H:%M:%S"),
            "raw": raw,
            "expected_cells": [dt.datetime.fromtimestamp(ts).strftime("%Y/%m/%d %H:%M:%S")]
                              + [cell_text(v) for v in shown],
        })
    con.commit()
    con.close()

    print(f"inserted {len(rows)} fixture row(s) into {db_path}")
    for r in rows:
        print(" ", r["time"], ",".join(r["expected_cells"][1:]))
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps({"db": str(db_path), "rows": rows}, ensure_ascii=False, indent=1),
                            encoding="utf-8")
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
