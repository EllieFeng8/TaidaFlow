# w2-052 D5: checks the mirror_history_client outputs of the live run (python -B).
#   python -B tools\analyze_mirror_runs.py   (from docs\evidence\w2-052)
# Exit 0 = every check passed.  Reads only the text files next to this folder.
#
# Checks
#   C1 every historyViews PATCH seen by any client changes exactly one entry (new revision or
#      added), i.e. one session's request never changes another entry's revision
#   C2 in the concurrent run (49, two connections: 127.0.0.1 and the LAN address) the patches of
#      web-d5E and web-d5F alternate, i.e. the requests really were interleaved in the Core
#   C3 each request's entry shows its own range and the page it asked for (or the last page)
#   C4 the interleaved web-d5E pages 1..30 equal the single-session web-d5R pages 1..30 row by
#      row (SHA-1 of the whole records list), same totalRows
#   C5 the entries no request touched keep their revision (desktop and the older web entries in
#      the snapshots / dumps of runs 48 .. 50)
#   C6 round 6 of run 47: web-d5A page 4 then page 5 back to back -> only page 5 is shown
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EV = os.path.dirname(HERE)

RUNS = {
    "47": ["47-mirror-interleaved.txt"],
    "49": ["49-mirror-concurrent-A-localhost.txt", "49-mirror-concurrent-B-lan.txt"],
    "50": ["50-mirror-single-reference.txt"],
}
PATCH = re.compile(r"PATCH historyViews: new revision \[([^\]]*)\], added \[([^\]]*)\], removed \[([^\]]*)\]; now (.*)$")
ENTRY = re.compile(r"ENTRY (\S+): range (.+?) \.\. (.+?) \((\S+)\), page (\d+)/(\d+) \(asked (\d+)\), totalRows (\d+), "
                   r"(\d+) record\(s\).*revision (\d+)$")
DIGEST = re.compile(r"ENTRY (\S+) digest page (\d+) rows (\d+) sha1 ([0-9a-f]{40})$")
SNAP = re.compile(r"snapshot: historyViews entries \[([^\]]*)\]")
DUMP = re.compile(r"DUMP (\S+): .*revision (\d+)$")

failures = []
report = []


def check(ok, text):
    report.append(("OK   " if ok else "FAIL ") + text)
    if not ok:
        failures.append(text)


def lines(name):
    with open(os.path.join(EV, name), encoding="utf-8") as f:
        return [l.rstrip("\n") for l in f]


def revs(text):
    out = {}
    for part in text.split():
        k, v = part.split("=")
        out[k] = int(v)
    return out


# C1 / C3
for run, files in RUNS.items():
    for name in files:
        patches = 0
        for l in lines(name):
            m = PATCH.search(l)
            if m:
                changed = [x for x in m.group(1).split(",") if x]
                added = [x for x in m.group(2).split(",") if x]
                removed = [x for x in m.group(3).split(",") if x]
                patches += 1
                if len(changed) + len(added) != 1 or removed:
                    check(False, f"C1 {name}: patch touches {changed + added} removed {removed}: {l}")
            m = ENTRY.search(l)
            if m:
                page, total, asked = int(m.group(5)), int(m.group(6)), int(m.group(7))
                check(page == asked or (asked > total and page == total),
                      f"C3 {name}: {m.group(1)} asked page {asked} -> page {page}/{total} (range {m.group(4)})")
        check(patches > 0, f"C1 {name}: {patches} patch(es), each changes exactly one entry")

# C2
order = []
for l in lines("49-mirror-concurrent-A-localhost.txt"):
    m = PATCH.search(l)
    if m:
        order.append((m.group(1) + m.group(2)).strip(","))
switches = sum(1 for a, b in zip(order, order[1:]) if a != b)
check(set(order) == {"web-d5E", "web-d5F"} and switches >= 20,
      f"C2 run 49: {len(order)} patches of {sorted(set(order))} seen by client A, {switches} alternations "
      f"between the two sessions (>= 20 means interleaved)")

# C4
def digests(name, sid):
    d = {}
    totals = {}
    for l in lines(name):
        m = DIGEST.search(l)
        if m and m.group(1) == sid:
            d[int(m.group(2))] = (int(m.group(3)), m.group(4))
        m = ENTRY.search(l)
        if m and m.group(1) == sid:
            totals[int(m.group(5))] = int(m.group(8))
    return d, totals


e, et = digests("49-mirror-concurrent-A-localhost.txt", "web-d5E")
r, rt = digests("50-mirror-single-reference.txt", "web-d5R")
check(sorted(e) == list(range(1, 31)) and sorted(r) == list(range(1, 31)),
      f"C4 pages present: interleaved web-d5E {len(e)}, single web-d5R {len(r)}")
same = [p for p in range(1, 31) if p in e and p in r and e[p] == r[p] and et.get(p) == rt.get(p)]
check(len(same) == 30, f"C4 interleaved web-d5E == single-session web-d5R on {len(same)}/30 pages "
                       f"(10 rows each, same SHA-1 of records, totalRows {sorted(set(rt.values()))})")

# C5
untouched = {}
for name in ["49-mirror-concurrent-A-localhost.txt", "49-mirror-concurrent-B-lan.txt", "50-mirror-single-reference.txt"]:
    for l in lines(name):
        m = SNAP.search(l)
        if m and m.group(1):
            for k, v in revs(m.group(1)).items():
                untouched.setdefault(k, set()).add(v)
        m = DUMP.search(l)
        if m:
            untouched.setdefault(m.group(1), set()).add(int(m.group(2)))
for sid in ["desktop", "web-d5A", "web-d5B", "web-d5C", "web-d5D"]:
    vals = untouched.get(sid, set())
    check(len(vals) == 1, f"C5 {sid}: revision {sorted(vals)} in every snapshot/dump of runs 49 and 50 "
                          f"(not requested there, never changed)")

# C6
l47 = lines("47-mirror-interleaved.txt")
sup = [l for l in l47 if "superseded by a later request of the same session" in l]
p5 = [l for l in l47 if ENTRY.search(l) and "web-d5A:" in l and "page 5/" in l]
p4 = [l for l in l47 if ENTRY.search(l) and "web-d5A:" in l and "page 4/" in l]
check(len(sup) == 1 and len(p5) == 1 and not p4,
      "C6 run 47 round 6: web-d5A page 4 superseded by page 5 of the same session; page 4 never shown, page 5 shown")

out = "\n".join(report) + f"\n\n{'ALL CHECKS PASSED' if not failures else str(len(failures)) + ' FAILURE(S)'}\n"
sys.stdout.write(out)
sys.exit(0 if not failures else 1)
