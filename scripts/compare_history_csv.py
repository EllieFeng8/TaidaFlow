#!/usr/bin/env python3
"""Verify exported History CSV files (desktop export and/or browser downloads).

For every CSV given:
  * starts with the UTF-8 BOM (EF BB BF) and decodes as UTF-8;
  * header row = "序號" + the 17 Core history titles (Core::loadHistoryRecords);
  * row count / every cell equals the History page content expected from the fixture JSON
    written by scripts/seed_history_sqlite.py (rows newest first, as HistoryPage.qml sorts
    them; numbers formatted with cellText() -> toFixed(2), missing value -> "—");
  * lines are CRLF separated (HistoryPage.qml joins with "\\r\\n").
Finally all files must be byte-identical (same SHA-256).

Usage: python scripts/compare_history_csv.py --fixture rows.json A.csv [B.csv ...]
Exit 0 only when every check passes.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import sys
from pathlib import Path

TITLES = ["序號", "時間",
          "TT-01 (°C)", "TT-02 (°C)", "TT-03 (°C)", "TT-04 (°C)",
          "PT-01 (bar)", "PT-02 (bar)", "PT-03 (bar)", "PT-04 (bar)",
          "PT-05 (bar)", "PT-06 (bar)", "PT-07 (bar)", "FM-01 (L/min)",
          "M1 (%)", "M2 (%)", "M3 (%)", "M4 (%)"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture", type=Path, required=True)
    ap.add_argument("csv", type=Path, nargs="+")
    args = ap.parse_args()

    fixture = json.loads(args.fixture.read_text(encoding="utf-8"))
    expected_rows = [[str(i + 1)] + r["expected_cells"]
                     for i, r in enumerate(reversed(fixture["rows"]))]
    ok = True
    hashes = {}
    for path in args.csv:
        data = path.read_bytes()
        hashes[path] = hashlib.sha256(data).hexdigest()
        problems = []
        if not data.startswith(b"\xef\xbb\xbf"):
            problems.append("no UTF-8 BOM")
        text = data[3:].decode("utf-8")
        if "\r\n" not in text or text.replace("\r\n", "").count("\n"):
            problems.append("line separator is not CRLF")
        rows = list(csv.reader(io.StringIO(text, newline="")))
        if not rows or rows[0] != TITLES:
            problems.append(f"header mismatch: {rows[0] if rows else None}")
        body = rows[1:]
        if len(body) != len(expected_rows):
            problems.append(f"row count {len(body)} != expected {len(expected_rows)}")
        for n, (got, exp) in enumerate(zip(body, expected_rows), start=1):
            if got != exp:
                problems.append(f"row {n}: {got} != {exp}")
        cjk = sorted({ch for ch in text if "一" <= ch <= "鿿"})
        print(f"== {path}")
        print(f"   bytes={len(data)} sha256={hashes[path]}")
        print(f"   bom={data[:3].hex()} header_cols={len(rows[0]) if rows else 0} data_rows={len(body)} "
              f"cjk_chars={''.join(cjk)} em_dash_cells={sum(c == chr(0x2014) for r in body for c in r)}")
        if problems:
            ok = False
            for p in problems:
                print(f"   FAIL {p}")
        else:
            print("   header/rows/cells match fixture: OK")
    distinct = set(hashes.values())
    print(f"distinct_sha256={len(distinct)} files={len(hashes)}")
    if len(distinct) != 1:
        ok = False
        print("FAIL files are not byte-identical")
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
