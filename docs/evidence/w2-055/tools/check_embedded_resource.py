#!/usr/bin/env python3
"""w2-055: prove that the built rcc resource (qrc_embedded_fonts.cpp) of each build tree
contains the regenerated App/fonts/*.ttf byte-for-byte, and that those embedded bytes have
the date/time picker and 顯示前一周 characters in their cmap.
(Copied from docs/evidence/w2-048/tools/check_embedded_resource.py; only CHARS changed: × added.)

  python -B docs/evidence/w2-055/tools/check_embedded_resource.py build/desktop build/wasm-release

Exit 0 when every build tree embeds both current TTF files unchanged (uncompressed rcc data)
and the embedded fonts contain every char of CHARS.
"""
import io
import re
import sys
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[4]
FONTS = ["TaidaFlowNotoSansTC-Regular.ttf", "TaidaFlowNotoSansTC-Bold.ttf"]
# w2-048 strings + the 15 chars of w1-047 D7(b) + the picker's ‹ › + × (w2-055, main 8a95a26)
CHARS = "顯示前一周" + "時間" + "確定" + "取消" + "年" + "月" + "日一二三四五六" \
        + "三五今六只周四圖年或按擇曆至鐘" + "‹›" + "×"
HEX = re.compile(rb"0x([0-9a-fA-F]{1,2})")


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    chars = "".join(dict.fromkeys(CHARS))
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
                missing = [c for c in chars if ord(c) not in cmap]
                line += f" cmap_has[{chars}]={'all' if not missing else 'MISSING ' + ''.join(missing)}"
                if missing:
                    rc = 1
            else:
                rc = 1
            print(line)
    print(f"rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
