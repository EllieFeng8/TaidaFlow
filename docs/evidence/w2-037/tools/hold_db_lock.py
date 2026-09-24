#!/usr/bin/env python3
"""w2-037 test helper: hold an EXCLUSIVE lock on the TaidaFlow monthly data file for N seconds,
so that the app's next alarm_history UPDATE fails ('database is locked') and the retry path of
Manager::checkDigitalInputAlarm can be observed.  Nothing is written: the transaction is
rolled back.  Every call appends start/end lines to input-log.txt.

  python hold_db_lock.py SECONDS [DB]
"""
from __future__ import annotations

import datetime as _dt
import glob
import os
import sqlite3
import sys
import time


def _now() -> str:
    return _dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 1
    seconds = float(argv[1])
    evidence = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.dirname(os.path.dirname(os.path.dirname(evidence)))  # taidaflow/
    db = argv[2] if len(argv) > 2 else sorted(glob.glob(os.path.join(
        root, "build", "runtime-cwd", "data", "sensor_*.sqlite")))[-1]
    log = os.path.join(evidence, "input-log.txt")
    con = sqlite3.connect(db, timeout=10, isolation_level=None)
    con.execute("BEGIN EXCLUSIVE")
    line = f"[{_now()}] hold_db_lock: EXCLUSIVE lock taken on {os.path.basename(db)} for {seconds:g} s"
    print(line, flush=True)
    with open(log, "a", encoding="utf-8") as f:
        f.write(line + "\n")
    time.sleep(seconds)
    con.execute("ROLLBACK")
    con.close()
    line = f"[{_now()}] hold_db_lock: lock released (ROLLBACK, nothing written)"
    print(line, flush=True)
    with open(log, "a", encoding="utf-8") as f:
        f.write(line + "\n")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv))
