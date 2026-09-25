#!/usr/bin/env python3
"""w2-048: diff the old/new charset.txt and check the new subset fonts' cmap.
(Copied from docs/evidence/w2-044/tools/check_cmap.py and adapted to the w2-048 char list.)

  python -B docs/evidence/w2-048/tools/check_cmap.py <old_fonts_dir> <new_fonts_dir>

Prints: chars added/removed in charset.txt, size of both TTFs before/after, and for every
required character (the 15 from the w1-047 report D7(b), the non-CJK ‹ › used by the date/time
picker and pagers, and every added char) whether it is in the cmap of each new TTF and of the
source font (C:/Windows/Fonts/NotoSansTC-VF.ttf). It also lists every charset char that the
source font does not have (those can never be in the subset).
Exit 0 when every required char that the source font has is present in both new TTFs.
"""
import sys
import unicodedata
from pathlib import Path

from fontTools.ttLib import TTFont

REQUIRED_W1047 = "三五今六只周四圖年或按擇曆至鐘"
NON_CJK = "\u2039\u203a"          # ‹ ›
PHRASES = ["顯示前一周", "時間", "確定", "取消", "年", "月", "日一二三四五六"]
SOURCE = Path("C:/Windows/Fonts/NotoSansTC-VF.ttf")
STYLES = ["Regular", "Bold"]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    old_dir, new_dir = Path(sys.argv[1]), Path(sys.argv[2])
    old = set((old_dir / "charset.txt").read_text(encoding="utf-8").rstrip("\n"))
    new = set((new_dir / "charset.txt").read_text(encoding="utf-8").rstrip("\n"))
    added = sorted(new - old)
    removed = sorted(old - new)
    print(f"charset.txt: old={len(old)} chars new={len(new)} chars")
    print(f"added ({len(added)}): {''.join(added)}")
    for ch in added:
        print(f"  + U+{ord(ch):04X} {ch} {unicodedata.name(ch, '?')}")
    print(f"removed ({len(removed)}): {''.join(removed)}")
    for ch in removed:
        print(f"  - U+{ord(ch):04X} {ch} {unicodedata.name(ch, '?')}")
    for style in STYLES:
        name = f"TaidaFlowNotoSansTC-{style}.ttf"
        a, b = (old_dir / name).stat().st_size, (new_dir / name).stat().st_size
        print(f"size {name}: {a} -> {b} bytes (delta {b - a:+d})")

    src = set(TTFont(str(SOURCE), lazy=True).getBestCmap())
    cmaps = {s: set(TTFont(str(new_dir / f"TaidaFlowNotoSansTC-{s}.ttf")).getBestCmap()) for s in STYLES}

    not_in_source = sorted(c for c in new if ord(c) not in src)
    print(f"charset chars NOT in source font NotoSansTC-VF ({len(not_in_source)}):")
    for ch in not_in_source:
        print(f"  U+{ord(ch):04X} {ch} {unicodedata.name(ch, '?')}")

    required = []
    for ch in REQUIRED_W1047 + NON_CJK + "".join(PHRASES) + "".join(added):
        if ch not in required:
            required.append(ch)
    rc = 0
    missing_expected = [c for c in REQUIRED_W1047 if c not in added]
    print(f"w1-047 D7(b) 15 chars all in 'added': {not missing_expected}"
          + ("" if not missing_expected else f"  <-- not added: {''.join(missing_expected)}"))
    if missing_expected:
        rc = 1
    print("cmap check (new fonts):")
    for ch in required:
        cp = ord(ch)
        in_src = cp in src
        row = " ".join(f"{s}={'yes' if cp in cmaps[s] else 'NO'}" for s in STYLES)
        flag = ""
        if in_src and not all(cp in cmaps[s] for s in STYLES):
            flag = "  <-- FAIL"
            rc = 1
        if not in_src:
            flag = "  (not in source font NotoSansTC-VF; cannot be subset)"
        print(f"  U+{cp:04X} {ch} in_charset={'yes' if ch in new else 'NO'} source={'yes' if in_src else 'no'} {row}{flag}")
    for phrase in PHRASES:
        ok = all(ord(c) in cmaps[s] for c in phrase for s in STYLES)
        print(f"phrase '{phrase}' fully covered in both styles: {ok}")
        if not ok:
            rc = 1
    print(f"rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
