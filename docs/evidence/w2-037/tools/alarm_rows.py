#!/usr/bin/env python3
"""w2-037 evidence helper: READ-ONLY dump of TaidaFlow alarm_history rows.

Opens build/runtime-cwd/data/sensor_<yyyymm>.sqlite with mode=ro (never writes) and prints
id, local time, sensor, message and Core status for rows with id > --after (default 0).

  python alarm_rows.py [--after N] [--db PATH] [--raw]   (--raw also prints the stored JSON)
Prints a last line 'max_id=N count=M'.
"""
from __future__ import annotations

import datetime as _dt
import glob
import json
import os
import sqlite3
import sys


def main(argv: list[str]) -> int:
    after = 0
    db = ""
    args = argv[1:]
    while args:
        if args[0] == "--after":
            after, args = int(args[1]), args[2:]
        elif args[0] == "--db":
            db, args = args[1], args[2:]
        elif args[0] == "--raw":
            args = args[1:]
        else:
            print(__doc__)
            return 1
    if not db:
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))))  # taidaflow/
        files = sorted(glob.glob(os.path.join(root, "build", "runtime-cwd", "data",
                                              "sensor_*.sqlite")))
        if not files:
            print("no sensor_*.sqlite found")
            return 2
        db = files[-1]
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    rows = con.execute("SELECT id, occurrence_time, reason FROM alarm_history "
                       "WHERE id > ? ORDER BY id", (after,)).fetchall()
    print(f"db={db} (read-only) rows with id > {after}:")
    for rid, t, reason in rows:
        when = _dt.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S") if t else "?"
        try:
            j = json.loads(reason)
            text = f"sensor={j.get('sensor')} message={j.get('alarmMessage')} status={j.get('status')}"
            if "resolved" in j:
                ra = j.get("resolvedAt")
                ra_text = _dt.datetime.fromtimestamp(ra).strftime("%H:%M:%S") if isinstance(ra, int) else ra
                text += (f" resolved={str(j.get('resolved')).lower()} resolvedAt={ra_text}"
                         f" resolvedDetail={j.get('resolvedDetail')}")
            if "--raw" in argv:
                text += f"  raw={reason}"
        except (ValueError, TypeError):
            text = f"reason={reason}"
        print(f"id={rid} time={when} {text}")
    max_id = con.execute("SELECT COALESCE(MAX(id), 0) FROM alarm_history").fetchone()[0]
    total = con.execute("SELECT COUNT(*) FROM alarm_history").fetchone()[0]
    print(f"max_id={max_id} count={total}")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv))
