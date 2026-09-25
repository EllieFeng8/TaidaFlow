#!/usr/bin/env python3
"""w2-044: diff the old/new charset.txt and check the new subset fonts' cmap.

  python -B docs/evidence/w2-044/tools/check_cmap.py <old_fonts_dir> <new_fonts_dir>

Prints: chars added/removed in charset.txt, size of both TTFs before/after, and for every
required character (the 9 from w2-041 + the chars of 排隊中 + every added char) whether it
is in the cmap of each new TTF and of the source font (C:/Windows/Fonts/NotoSansTC-VF.ttf).
Exit 0 when every required char that the source font has is present in both new TTFs.
"""
import sys
import unicodedata
from pathlib import Path

from fontTools.ttLib import TTFont

REQUIRED_W2041 = "–✕月求為要跨送隊"
PHRASE = "排隊中"
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
    for style in STYLES:
        name = f"TaidaFlowNotoSansTC-{style}.ttf"
        a, b = (old_dir / name).stat().st_size, (new_dir / name).stat().st_size
        print(f"size {name}: {a} -> {b} bytes (delta {b - a:+d})")

    src = set(TTFont(str(SOURCE), lazy=True).getBestCmap())
    cmaps = {s: set(TTFont(str(new_dir / f"TaidaFlowNotoSansTC-{s}.ttf")).getBestCmap()) for s in STYLES}
    required = []
    for ch in REQUIRED_W2041 + PHRASE + "".join(added):
        if ch not in required:
            required.append(ch)
    rc = 0
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
    phrase_ok = all(ord(c) in cmaps[s] for c in PHRASE for s in STYLES)
    print(f"phrase '{PHRASE}' fully covered in both styles: {phrase_ok}")
    if not phrase_ok:
        rc = 1
    print(f"rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
