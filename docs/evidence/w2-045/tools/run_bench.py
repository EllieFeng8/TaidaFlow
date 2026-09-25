"""w2-045 D3: runs w2045_bench(.exe) / w2045_bench_old(.exe) scenarios and writes the evidence.

  python -B docs/evidence/w2-045/tools/run_bench.py [--out docs/evidence/w2-045/30-bench]

Needs: build/w2-045-qtest/w2045_bench.exe (+ w2045_bench_old.exe for the before/after rows),
built by docs/evidence/w2-045/tools/run-qtest.bat, and the 12-month bench data
build/w2-045-bench/data (make_bench_db.py).  Every run is a new process (= application restart:
empty count cache).  "cold" runs drop the month files from the Windows file cache first
(FILE_FLAG_NO_BUFFERING open inside the bench, before SqlManager opens them).

Writes <out>-raw.txt (every output line of every run) and <out>-summary.txt (tables).
Exit 0 when every run exited 0 (all page checks equal to the OFFSET reference) and, for the
new implementation, no SqlManager step >= 40 ms and every scenario's longest blocking save is
at most 20 ms above the idle baseline with the same save interval.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]           # taidaflow/
BUILD = ROOT / "build" / "w2-045-qtest"
DATA = ROOT / "build" / "w2-045-bench" / "data"
QT_BIN = r"C:\Qt\6.8.3\msvc2022_64\bin"

ap = argparse.ArgumentParser()
ap.add_argument("--out", default=str(ROOT / "docs" / "evidence" / "w2-045" / "30-bench"))
ap.add_argument("--impl", choices=("both", "new", "old"), default="both",
                help="which build(s) to run (old = pre-w2-045 SqlManager, w2045_bench_old.exe)")
args = ap.parse_args()

# (impl, scenario, cold, save_ms)
RUNS = [("new", "long", False, 1000)]                  # warm-up: brings the files into the OS cache
PLAN = []
for impl in ("new", "old"):
    for save_ms in (1000, 20):                          # every second (as Manager) and a 20 ms stress load
        if impl == "new":
            PLAN.append((impl, "idle", False, save_ms))
        for cold in (False, True):
            # "long" twice: the second process is the application restart
            for scenario in ("long", "long", "unbounded", "deep", "last"):
                PLAN.append((impl, scenario, cold, save_ms))


def run(impl, scenario, cold, save_ms, raw):
    exe = BUILD / ("w2045_bench.exe" if impl == "new" else "w2045_bench_old.exe")
    cmd = [str(exe), scenario, "--save-ms", str(save_ms)] + (["--cold"] if cold else [])
    if scenario == "idle":
        cmd += ["--idle-s", "15"]
    env = dict(os.environ)
    env["PATH"] = QT_BIN + os.pathsep + env["PATH"]
    for attempt in range(10):
        try:
            p = subprocess.run(cmd, cwd=BUILD, env=env, capture_output=True, text=True, timeout=900)
            break
        except PermissionError:                         # a freshly linked exe can be locked (virus scan)
            if attempt == 9:
                raise
            time.sleep(3)
    lines = [l for l in (p.stdout + p.stderr).splitlines() if "Schema file not found" not in l]
    raw.write(f"\n### {' '.join(cmd[1:])}  impl={impl}  exit={p.returncode}\n")
    raw.write("\n".join(lines) + "\n")
    raw.flush()
    return p.returncode, lines


def kv(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def main():
    if not (BUILD / "w2045_bench.exe").exists() or not DATA.exists():
        print("build w2045_bench.exe (run-qtest.bat) and make the bench data first")
        return 2
    free = shutil.disk_usage(DATA).free / 2**30
    size = sum(f.stat().st_size for f in DATA.glob("sensor_*.sqlite")) / 2**30
    out = Path(args.out)
    results = []
    failures = []
    with open(str(out) + "-raw.txt", "w", encoding="utf-8") as raw:
        raw.write(f"bench data {DATA}: {len(list(DATA.glob('sensor_*.sqlite')))} month files, {size:.2f} GiB; "
                  f"free disk space {free:.1f} GiB\n")
        for impl, scenario, cold, save_ms in RUNS + PLAN:
            if impl == "old" and not (BUILD / "w2045_bench_old.exe").exists():
                continue
            if args.impl != "both" and impl != args.impl:
                continue
            rc, lines = run(impl, scenario, cold, save_ms, raw)
            print(f"{impl:3s} {scenario:9s} cold={int(cold)} save_ms={save_ms:4d} exit={rc}", flush=True)
            if rc != 0:
                failures.append(f"{impl} {scenario} cold={cold} save_ms={save_ms} exit={rc}")
            reqs = [kv(l) | {"name": l.split()[2]} for l in lines if l.startswith("REQ ")]
            scen = [kv(l) for l in lines if l.startswith("SCENARIO ")]
            checks = [kv(l) for l in lines if l.startswith("CHECKS ")]
            results.append({"impl": impl, "scenario": scenario, "cold": cold, "save_ms": save_ms, "rc": rc,
                             "reqs": reqs, "scen": scen[0] if scen else {}, "checks": checks[0] if checks else {}})

    measured = results[1:]                              # without the warm-up
    idle = {r["save_ms"]: float(r["scen"].get("save_max_ms", "nan")) for r in measured if r["scenario"] == "idle"}
    idle_wait = {r["save_ms"]: float(r["scen"].get("wait_max_ms", "nan")) for r in measured if r["scenario"] == "idle"}
    rows = []
    over40 = 0
    over20 = []
    seen = {}
    for r in measured:
        if r["scenario"] == "idle":
            continue
        key = (r["impl"], r["scenario"], r["cold"], r["save_ms"])
        seen[key] = seen.get(key, 0) + 1
        label = r["scenario"] + ("" if seen[key] == 1 else " (restart)")
        s = r["scen"]
        base = idle.get(r["save_ms"], float("nan"))
        save_max = float(s.get("save_max_ms", "nan"))
        delta = save_max - base
        if r["impl"] == "new":
            over40 += int(s.get("steps_over_40ms", "0"))
            if delta > 20.0:
                over20.append(f"{label} cold={r['cold']} save_ms={r['save_ms']}: +{delta:.2f} ms")
        for q in r["reqs"]:
            rows.append((r["impl"], label, "cold" if r["cold"] else "hot", r["save_ms"], q["name"], q.get("method", ""),
                         q.get("request_ms", ""), q.get("steps", ""), q.get("step_max_ms", ""), q.get("step_p90_ms", ""),
                         q.get("saves_during", ""), q.get("save_max_ms_during", ""), q.get("wait_max_ms_during", ""),
                         q.get("tick_gap_max_ms_during", ""), q.get("total_rows", "")))
        rows.append((r["impl"], label, "cold" if r["cold"] else "hot", r["save_ms"], "== scenario", "",
                     s.get("request_max_ms", ""), s.get("steps", ""), s.get("step_max_ms", ""), s.get("step_p90_ms", ""),
                     s.get("saves", ""), f"{save_max:.2f} (idle {base:.2f}, {delta:+.2f})", s.get("wait_max_ms", ""),
                     s.get("tick_gap_max_ms", ""), f"checks {r['checks'].get('total', '-')}/mismatch {r['checks'].get('mismatches', '-')}"))

    head = ("impl", "scenario", "cache", "save_ms", "request", "method", "request_ms", "steps", "step_max_ms",
            "step_p90_ms", "saves_during", "save_max_ms_during", "queue_wait_max_ms", "tick_gap_max_ms", "rows/checks")
    widths = [max(len(str(x[i])) for x in rows + [head]) for i in range(len(head))]
    with open(str(out) + "-summary.txt", "w", encoding="utf-8") as f:
        f.write(f"w2-045 D3 measurements; bench data {size:.2f} GiB in {DATA} (12 month files, 1 row/s, "
                f"~31.0 M rows); free disk space {free:.1f} GiB\n")
        f.write("idle baseline (no History request, 15 s), longest blocking save / longest queue wait: "
                + ", ".join(f"save every {k} ms: {v:.2f} / {idle_wait.get(k, float('nan')):.2f} ms"
                            for k, v in sorted(idle.items())) + "\n")
        f.write("request_ms = main thread post -> result; steps/step_max/step_p90 = SqlManager thread (old: the whole "
                "request is one step); saves_during / save_max_ms_during = blocking saveSensorData calls of the main "
                "thread overlapping the request (whole blocking call); queue_wait = blocking no-op call into the "
                "SqlManager thread made just before each save (time the thread was busy with other work, e.g. a History "
                "step); tick_gap = longest main-thread 1 ms timer gap.\n\n")
        for row in [head] + rows:
            f.write("  ".join(str(v).ljust(w) for v, w in zip(row, widths)).rstrip() + "\n")
        f.write(f"\nnew implementation: SqlManager steps >= 40 ms: {over40}; scenarios whose longest save exceeds the "
                f"idle baseline by more than 20 ms: {len(over20)} {over20}\n")
        f.write(f"runs with a non-zero exit (page check mismatch or error): {len(failures)} {failures}\n")
    print(open(str(out) + "-summary.txt", encoding="utf-8").read())
    return 0 if not failures and over40 == 0 and not over20 else 1


if __name__ == "__main__":
    sys.exit(main())
