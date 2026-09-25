#!/usr/bin/env python3
"""w2-044: prove that the built rcc resource (qrc_embedded_fonts.cpp) of each build tree
contains the regenerated App/fonts/*.ttf byte-for-byte, and that those embedded bytes have
the new characters in their cmap.

  python -B docs/evidence/w2-044/tools/check_embedded_resource.py build/desktop build/wasm-release

Exit 0 when every build tree embeds both current TTF files unchanged (uncompressed rcc data)
and the embedded fonts contain 排隊中 and the 8 new characters available in the source font.
"""
import io
import re
import sys
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[4]
FONTS = ["TaidaFlowNotoSansTC-Regular.ttf", "TaidaFlowNotoSansTC-Bold.ttf"]
CHARS = "排隊中–月求為要跨送"
HEX = re.compile(rb"0x([0-9a-fA-F]{1,2})")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    rc = 0
    for tree in sys.argv[1:]:
        src = ROOT / tree / "App" / ".qt" / "rcc" / "qrc_embedded_fonts.cpp"
        text = src.read_bytes()
        start = text.find(b"qt_resource_data[]")
        end = text.find(b"};", start)
        blob = bytes(int(m.group(1), 16) for m in HEX.finditer(text[start:end]))
        print(f"{tree}: {src.relative_to(ROOT)} resource_data_bytes={len(blob)}")
        for name in FONTS:
            data = (ROOT / "App" / "fonts" / name).read_bytes()
            pos = blob.find(data)
            ok = pos >= 0
            line = f"  {name}: size={len(data)} embedded_verbatim={ok}"
            if ok:
                cmap = TTFont(io.BytesIO(blob[pos:pos + len(data)])).getBestCmap()
                missing = [c for c in CHARS if ord(c) not in cmap]
                line += f" cmap_has[{CHARS}]={'all' if not missing else 'MISSING ' + ''.join(missing)}"
                if missing:
                    rc = 1
            else:
                rc = 1
            print(line)
    print(f"rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
