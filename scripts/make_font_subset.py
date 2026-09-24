#!/usr/bin/env python3
"""Build the embedded CJK font subset used by TaidaFlow (WebAssembly has no system fonts).

What it does
  1. Scans the project's QML / JS / C++ sources (App, Core, TaidaFlow, TaidaFlowContent,
     Dependencies) and collects every non-ASCII character that appears in them.
  2. Adds printable ASCII (U+0020..U+007E) and a small fixed set of common CJK
     punctuation so that numbers / labels / later edits keep rendering.
  3. Instantiates the Noto Sans TC variable font (OFL-1.1) at wght=400 and wght=700
     and subsets both instances to that character set (static TTF, no hinting).
  4. Verifies that every collected character supported by the source font is present
     in both outputs, and fails (exit 1) if a CJK character is not covered at all.

Usage
  python scripts/make_font_subset.py                 # build + verify (default source font)
  python scripts/make_font_subset.py --check         # verify existing outputs only
  python scripts/make_font_subset.py --source NotoSansTC[wght].ttf   # Google Fonts download

Source font
  Default: C:/Windows/Fonts/NotoSansTC-VF.ttf (Noto Sans TC 2.004 variable, OFL-1.1, ships
  with Windows 11). The same family is available from https://fonts.google.com/noto/specimen/Noto+Sans+TC
  (file NotoSansTC[wght].ttf); either works as --source.

Requires: fontTools (pip install fonttools). Exit code 0 = outputs written and verified.
"""
from __future__ import annotations

import argparse
import sys
import unicodedata
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "App" / "fonts"
FAMILY = "TaidaFlow Noto Sans TC"
PS_FAMILY = "TaidaFlowNotoSansTC"
DEFAULT_SOURCE = Path("C:/Windows/Fonts/NotoSansTC-VF.ttf")
SCAN_DIRS = ["App", "Core", "TaidaFlow", "TaidaFlowContent", "Dependencies"]
SCAN_EXTS = {".qml", ".js", ".mjs", ".cpp", ".cc", ".cxx", ".h", ".hpp"}
# Common CJK / fullwidth punctuation and symbols kept even if not (yet) used in sources.
EXTRA = "，。、：；！？（）「」『』《》〈〉【】…—～％＋－／＝　·°℃"
INSTANCES = [("Regular", 400), ("Bold", 700)]


def output_path(style: str) -> Path:
    return OUT_DIR / f"{PS_FAMILY}-{style}.ttf"


def is_cjk(ch: str) -> bool:
    cp = ord(ch)
    return (
        0x2E80 <= cp <= 0x9FFF        # radicals, CJK symbols/punct, kana, bopomofo, CJK unified
        or 0xF900 <= cp <= 0xFAFF     # compatibility ideographs
        or 0xFE30 <= cp <= 0xFE4F     # CJK compatibility forms
        or 0xFF00 <= cp <= 0xFFEF     # half/fullwidth forms
        or 0x20000 <= cp <= 0x2FA1F   # ext B+ / compat supplement
    )


def collect_chars() -> tuple[set[str], list[Path]]:
    chars: set[str] = set()
    files: list[Path] = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.suffix.lower() not in SCAN_EXTS or not p.is_file():
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            files.append(p)
            for ch in text:
                if ord(ch) > 0x7E and ch != "\ufffd" and unicodedata.category(ch)[0] != "C":
                    chars.add(ch)
    chars.update(chr(c) for c in range(0x20, 0x7F))
    chars.update(EXTRA)
    return chars, files


def cmap_of(font: TTFont) -> set[int]:
    return set(font.getBestCmap().keys())


def set_names(font: TTFont, style: str, weight: int) -> None:
    name = font["name"]
    # Drop variable-font / typographic names that would confuse the static instance.
    for nid in (16, 17, 21, 22, 25):
        name.removeNames(nameID=nid)
    full = f"{FAMILY} {style}" if style != "Regular" else FAMILY
    for nid, value in (
        (1, FAMILY),
        (2, style),
        (3, f"{PS_FAMILY}-{style};subset-for-TaidaFlow"),
        (4, full),
        (6, f"{PS_FAMILY}-{style}"),
    ):
        name.setName(value, nid, 3, 1, 0x409)
        name.setName(value, nid, 1, 0, 0)
    os2 = font["OS/2"]
    os2.usWeightClass = weight
    bold = style == "Bold"
    os2.fsSelection = (os2.fsSelection & ~0b1100001) | (0b100000 if bold else 0b1000000)
    font["head"].macStyle = 0b1 if bold else 0


def build(source: Path, chars: set[str]) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for style, weight in INSTANCES:
        vf = TTFont(str(source))
        if "fvar" in vf:
            static = instancer.instantiateVariableFont(vf, {"wght": weight}, updateFontNames=False)
        else:
            static = vf
        opts = subset.Options()
        opts.hinting = False
        opts.name_IDs = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
        opts.name_languages = [0x409]
        opts.notdef_outline = True
        opts.layout_features = ["*"]
        opts.drop_tables += ["BASE", "STAT", "vhea", "vmtx", "DSIG"]
        sub = subset.Subsetter(opts)
        sub.populate(unicodes=[ord(c) for c in sorted(chars)])
        sub.subset(static)
        set_names(static, style, weight)
        out = output_path(style)
        static.save(str(out))
        print(f"wrote {out.relative_to(ROOT)}  {out.stat().st_size} bytes  wght={weight}")


def verify(source: Path, chars: set[str]) -> int:
    src_cmap = cmap_of(TTFont(str(source), lazy=True)) if source.exists() else None
    required = {ord(c) for c in chars}
    status = 0
    unsupported = sorted(c for c in required if src_cmap is not None and c not in src_cmap)
    for cp in unsupported:
        ch = chr(cp)
        tag = "ERROR (CJK not in source font)" if is_cjk(ch) else "note (not in source font, skipped)"
        print(f"  {tag}: U+{cp:04X} {unicodedata.name(ch, '?')}")
        if is_cjk(ch):
            status = 1
    expected = required - set(unsupported)
    total = 0
    for style, _ in INSTANCES:
        out = output_path(style)
        if not out.exists():
            print(f"MISSING output {out}")
            return 1
        f = TTFont(str(out))
        cmap = cmap_of(f)
        missing = sorted(expected - cmap)
        fam = f["name"].getDebugName(1)
        sub_ = f["name"].getDebugName(2)
        size = out.stat().st_size
        total += size
        print(f"verify {out.name}: family='{fam}' style='{sub_}' weight={f['OS/2'].usWeightClass} "
              f"glyphs={len(f.getGlyphOrder())} size={size} missing={len(missing)}")
        for cp in missing[:20]:
            print(f"  missing U+{cp:04X} {chr(cp)}")
        if missing:
            status = 1
    cjk = sorted(c for c in chars if is_cjk(c))
    print(f"chars: total={len(chars)} cjk={len(cjk)} total_font_bytes={total}")
    return status


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    ap.add_argument("--check", action="store_true", help="only verify existing outputs")
    args = ap.parse_args()

    chars, files = collect_chars()
    print(f"scanned {len(files)} source files under {', '.join(SCAN_DIRS)}")
    charset_file = OUT_DIR / "charset.txt"
    ordered = "".join(sorted(chars))
    if args.check:
        if charset_file.exists():
            recorded = charset_file.read_text(encoding="utf-8").rstrip("\n")
            if recorded != ordered:
                new = sorted(set(ordered) - set(recorded))
                print(f"charset changed since last build ({len(new)} new chars: {''.join(new)[:60]}) "
                      f"-> re-run without --check")
                return 1
    else:
        if not args.source.exists():
            print(f"source font not found: {args.source}")
            return 2
        build(args.source, chars)
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        charset_file.write_text(ordered + "\n", encoding="utf-8")
    return verify(args.source, chars)


if __name__ == "__main__":
    sys.exit(main())
