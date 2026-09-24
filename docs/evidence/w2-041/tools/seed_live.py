#!/usr/bin/env python3
"""w2-041 live-run TEST FIXTURE: put the 30-day bench month files into the desktop Core's own
data folder, then put the original back.

  python seed_live.py seed     back up build/runtime-cwd/data/sensor_202609.sqlite to
                               sensor_202609.sqlite.w2-041-before.bak (and TaidaFlowSettings.ini to
                               TaidaFlowSettings.ini.w2-041-before.bak), refuse if 202607/202608 already
                               exist, then copy build/w2-041-bench/data/sensor_2026{07,08,09}.sqlite
                               (made by make_bench_db.py) into build/runtime-cwd/data/.
  python seed_live.py restore  delete the copied 202607/202608 files, copy the .bak back over
                               sensor_202609.sqlite (drops the bench rows and anything the app wrote
                               during the run), delete the .bak; compare the settings file with its .bak.
  python seed_live.py info     row count / min / max of every month file (read-only).

Refuses to run while TaidaFlowApp.exe is running.  Only git-ignored build/ files are touched.
The app then reads the files through its normal path (no product code involved).
"""
import filecmp
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]          # taidaflow/
DATA = ROOT / "build" / "runtime-cwd" / "data"
BENCH = ROOT / "build" / "w2-041-bench" / "data"
SEP = DATA / "sensor_202609.sqlite"
BAK = SEP.with_name(SEP.name + ".w2-041-before.bak")
INI = ROOT / "build" / "runtime-cwd" / "TaidaFlowSettings.ini"
INI_BAK = INI.with_name(INI.name + ".w2-041-before.bak")
ADDED = ["sensor_202607.sqlite", "sensor_202608.sqlite"]


def app_running() -> bool:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq TaidaFlowApp.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "TaidaFlowApp.exe" in out


def info() -> None:
    for f in sorted(DATA.glob("sensor_*.sqlite")):
        d = sqlite3.connect(f"file:{f.as_posix()}?mode=ro", uri=True)
        n, lo, hi = d.execute("SELECT COUNT(*), MIN(timestamp), MAX(timestamp) FROM sensor_data").fetchone()
        d.close()
        print(f"  {f.name}: rows={n} min={lo} max={hi} size={f.stat().st_size}")
    for f in sorted(DATA.glob("*.bak")):
        print(f"  backup present: {f.name} size={f.stat().st_size}")


cmd = sys.argv[1] if len(sys.argv) > 1 else "info"
if cmd in ("seed", "restore") and app_running():
    print("TaidaFlowApp.exe is running - close it first")
    sys.exit(2)
if cmd == "seed":
    if BAK.exists() or any((DATA / n).exists() for n in ADDED):
        print("already seeded (backup or 202607/202608 present) - restore first")
        sys.exit(2)
    print("before:"); info()
    shutil.copy2(SEP, BAK)
    if INI.exists():
        shutil.copy2(INI, INI_BAK)
    for name in ADDED + ["sensor_202609.sqlite"]:
        shutil.copy2(BENCH / name, DATA / name)
    print("after seed:"); info()
elif cmd == "restore":
    if not BAK.exists():
        print("no backup - nothing to restore")
        sys.exit(2)
    print("before restore:"); info()
    for name in ADDED:
        (DATA / name).unlink(missing_ok=True)
    shutil.copy2(BAK, SEP)
    BAK.unlink()
    if INI_BAK.exists():
        same = filecmp.cmp(INI, INI_BAK, shallow=False)
        print(f"TaidaFlowSettings.ini unchanged during the run: {same}")
        INI_BAK.unlink()
    print("after restore:"); info()
else:
    info()
