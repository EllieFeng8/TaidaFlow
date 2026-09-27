#!/usr/bin/env python3
"""w2-053 evidence helper: compare two alarm_rows.py dumps (before / after).

  python -B compare_dumps.py <before.txt> <after.txt>

Prints: rows that changed (with old and new text), rows added, rows removed, and checks that
the changed rows are exactly the DI rows with status=異常 that were not resolved in <before>
(and that each of them is resolved in <after>).  Exit 0 when that holds and nothing was removed.
"""
import re
import sys

ROW = re.compile(r"^id=(\d+) (.*)$")


def load(path):
    rows = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = ROW.match(line.rstrip("\n"))
            if m:
                rows[int(m.group(1))] = m.group(2)
    return rows


def main(argv):
    sys.stdout.reconfigure(encoding="utf-8")
    before, after = load(argv[1]), load(argv[2])
    unresolved = sorted(i for i, t in before.items()
                        if re.search(r"sensor=DI\d ", t) and "status=異常" in t and "resolved=true" not in t)
    changed = sorted(i for i in before if i in after and before[i] != after[i])
    added = sorted(i for i in after if i not in before)
    removed = sorted(i for i in before if i not in after)
    print(f"before: {len(before)} rows, after: {len(after)} rows")
    print(f"unresolved DI 異常 rows in before ({len(unresolved)}): {unresolved}")
    print(f"changed rows ({len(changed)}): {changed}")
    for i in changed:
        print(f"  id={i}\n    before: {before[i]}\n    after:  {after[i]}")
    print(f"added rows ({len(added)}):")
    for i in added:
        print(f"  id={i} {after[i]}")
    print(f"removed rows ({len(removed)}): {removed}")
    ok = changed == unresolved and all("resolved=true" in after[i] for i in changed) and not removed
    still = sorted(i for i, t in after.items()
                   if re.search(r"sensor=DI\d ", t) and "status=異常" in t and "resolved=true" not in t)
    print(f"unresolved DI 異常 rows in after ({len(still)}): {still}")
    print(f"changed == unresolved-before, all resolved, none removed: {ok}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
