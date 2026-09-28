"""w2-063: write <clone>/CMakeUserPresets.json from the template in docs/BUILD.md section 2.6.

  python -B fc-06-make-user-presets.py <clone dir> <tools dir> <emsdk dir>

Takes the first ```json block after the heading "### 2.6" of <clone>/docs/BUILD.md unchanged, replaces
"D:/Qt" with "<tools dir>/Qt" and "D:/emsdk" with "<emsdk dir>" (forward slashes), checks that it
is valid JSON and writes it. Exit 0 on success.
"""
import json
import re
import sys
from pathlib import Path

clone, tools, emsdk = (Path(a).resolve() for a in sys.argv[1:4])
doc = (clone / "docs" / "BUILD.md").read_text(encoding="utf-8")
sec = doc[doc.index("### 2.6"):]
m = re.search(r"```json\n(.*?)\n```", sec, re.S)
text = m.group(1)
t = tools.as_posix()
text = text.replace("D:/Qt", t + "/Qt").replace("D:/emsdk", emsdk.as_posix())
data = json.loads(text)
names = [p["name"] for p in data["configurePresets"]]
(clone / "CMakeUserPresets.json").write_text(text + "\n", encoding="utf-8")
print(f"wrote {clone / 'CMakeUserPresets.json'} presets={names} tools={t} emsdk={emsdk.as_posix()}")
