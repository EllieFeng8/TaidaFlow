#!/usr/bin/env python3
"""w2-058 D3: static checks of the built WebAssembly page (no browser, no screenshots).

  python -B docs/evidence/w2-058/check_shell_page.py build/wasm-release/TaidaFlowApp.html [more.html ...]
         [--node C:/tools/emsdk/node/16.20.0_64bit/bin/node.exe]

For every page:
  * HTML: parses with html.parser; every non-void element is closed in order (strict stack check);
    exactly one <html lang="zh-Hant-TW">, <title> == "TaidaFlow", the Qt viewport meta,
    <div id="screen">, the loader (#tf-loader, #tf-status, .tf-title "TAIDAFLOW"), 6 faces;
  * placeholders: no @NAME@ left; entryFunction: window.<name>, and containerElements: [screen]
    present; <script src="<APPNAME>.js"> and qtloader.js loaded, the entry name matches the one
    exported by <APPNAME>.js (EXPORT_NAME of the Emscripten module) when that file is next to the page;
  * Qt default page gone: no qtlogo, "Loading...", "Qt for WebAssembly", "Application exit";
  * no external resource: no <link>, no http(s):// or // URLs, no @import, no url(...);
  * CSS: braces balanced, every declaration is "property: value" with a known property name,
    the design values are there (96px cube, 48px/80px translateZ, 8s/3s/2s animations,
    perspective 1200px, colours #22d3ee #c084fc #818cf8 #67e8f9 #94a3b8 #FF5964 #0B1527,
    backdrop-filter blur(2px), prefers-reduced-motion);
  * texts: the PM wording of D1 (stage texts, error texts, noscript text);
  * JS: the inline script passes `node --check` (syntax) when node is available.
Exit 0 when every check passes, 1 otherwise.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from html.parser import HTMLParser
from pathlib import Path

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source",
        "track", "wbr"}
CSS_PROPS = {
    "padding", "margin", "overflow", "height", "width", "background", "position", "inset",
    "z-index", "display", "flex-direction", "align-items", "justify-content", "gap",
    "box-sizing", "perspective", "font-family", "transform-style", "animation", "border-radius",
    "filter", "box-shadow", "transform", "border", "opacity", "-webkit-backdrop-filter",
    "backdrop-filter", "background-color", "border-color", "bottom", "margin-top", "max-width",
    "text-align", "font-size", "line-height", "font-weight", "letter-spacing", "text-transform",
    "color", "animation-play-state",
}
TEXTS = [
    "TAIDAFLOW",
    "系統載入中,首次開啟約需數秒…",
    "啟動中…",
    "無法載入系統,請確認網路連線後重新整理頁面(F5)。",
    "此瀏覽器不支援本系統,請使用最新版 Chrome 或 Edge。",
    "請啟用 JavaScript 後重新整理頁面。",
    "系統已結束(代碼 ${exitData.code}),請重新整理頁面(F5)。",
    "系統已結束,請重新整理頁面(F5)。",
]
DESIGN = [
    "width: 96px", "height: 96px", "translateZ(48px)", "translateZ(80px)", "perspective: 1200px",
    "tf-cube-spin 8s linear infinite", "tf-breathe 3s ease-in-out infinite",
    "tf-pulse-fast 2s ease-in-out infinite", "tf-shadow-breathe 3s ease-in-out infinite",
    "rotateX(360deg) rotateY(360deg)", "border-color: rgba(255, 255, 255, 0.8)",
    "#22d3ee", "#c084fc", "#818cf8", "#67e8f9", "#94a3b8", "#FF5964", "#0B1527",
    "backdrop-filter: blur(2px)", "letter-spacing: 0.3em", "text-transform: uppercase",
    "prefers-reduced-motion: reduce", "rgba(34, 211, 238, 0.4)", "rgba(168, 85, 247, 0.4)",
    "rgba(99, 102, 241, 0.4)", "filter: blur(12px)", "filter: blur(24px)", "bottom: -80px",
    '"Segoe UI", "Microsoft JhengHei", sans-serif',
]


class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack: list[str] = []
        self.errors: list[str] = []
        self.tags: list[tuple[str, dict]] = []
        self.styles: list[str] = []
        self.scripts: list[str] = []
        self.text_of: dict[str, str] = {}
        self._capture: str | None = None
        self._buf: list[str] = []

    def handle_starttag(self, tag, attrs):
        a = {k: (v or "") for k, v in attrs}
        self.tags.append((tag, a))
        if tag not in VOID:
            self.stack.append(tag)
        if tag in ("style", "script", "title", "h1") or a.get("id") == "tf-status":
            self._capture = a.get("id") or tag
            self._buf = []

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID and self.stack and self.stack[-1] == tag:
            self.stack.pop()

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.errors.append(f"unexpected </{tag}> (open: {self.stack[-3:]}) at line {self.getpos()[0]}")
            if tag in self.stack:
                while self.stack and self.stack.pop() != tag:
                    pass
            return
        self.stack.pop()
        if self._capture:
            text = "".join(self._buf)
            if tag == "style":
                self.styles.append(text)
            elif tag == "script":
                self.scripts.append(text)
            elif self._capture not in self.text_of:
                self.text_of[self._capture] = text
            self._capture = None

    def handle_data(self, data):
        if self._capture:
            self._buf.append(data)


def css_check(css: str) -> list[str]:
    errs = []
    body = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    if body.count("{") != body.count("}"):
        errs.append(f"CSS braces unbalanced: {{={body.count('{')} }}={body.count('}')}")
    depth = 0
    decl_buf = ""
    for ch in body:
        if ch == "{":
            depth += 1
            decl_buf = ""
        elif ch == "}":
            decl_buf_s = decl_buf.strip()
            if decl_buf_s:
                errs += check_decls(decl_buf_s)
            depth -= 1
            decl_buf = ""
            if depth < 0:
                errs.append("CSS: '}' without '{'")
                depth = 0
        else:
            decl_buf += ch
    return errs


def check_decls(block: str) -> list[str]:
    errs = []
    for decl in block.split(";"):
        decl = decl.strip()
        if not decl or ":" not in decl and "{" not in decl and re.match(r"^[\d%, ]+$", decl):
            continue
        if ":" not in decl:
            errs.append(f"CSS: not a declaration: {decl[:60]!r}")
            continue
        prop, value = (s.strip() for s in decl.split(":", 1))
        if prop not in CSS_PROPS:
            errs.append(f"CSS: unknown property {prop!r}")
        if not value:
            errs.append(f"CSS: empty value for {prop!r}")
    return errs


def check(path: Path, node: str | None) -> list[str]:
    errs: list[str] = []
    raw = path.read_text(encoding="utf-8")
    p = Page()
    p.feed(raw)
    p.close()
    errs += p.errors
    if p.stack:
        errs.append(f"unclosed elements: {p.stack}")

    htmls = [a for t, a in p.tags if t == "html"]
    if len(htmls) != 1 or htmls[0].get("lang") != "zh-Hant-TW":
        errs.append(f"<html lang> = {[a.get('lang') for a in htmls]}")
    if p.text_of.get("title", "").strip() != "TaidaFlow":
        errs.append(f"title = {p.text_of.get('title')!r}")
    if not any(t == "meta" and a.get("name") == "viewport"
               and a.get("content") == "width=device-width, height=device-height, user-scalable=0"
               for t, a in p.tags):
        errs.append("Qt viewport meta missing")
    ids = [a.get("id") for _, a in p.tags if a.get("id")]
    for need in ("screen", "tf-loader", "tf-status"):
        if ids.count(need) != 1:
            errs.append(f"id {need!r} count = {ids.count(need)}")
    faces = sum(1 for _, a in p.tags if "tf-face" in a.get("class", "").split())
    if faces != 6:
        errs.append(f"faces = {faces}")
    if p.text_of.get("h1", "").strip() != "TAIDAFLOW":
        errs.append(f"title text = {p.text_of.get('h1')!r}")

    left = re.findall(r"@[A-Za-z_][A-Za-z0-9_]*@", raw)
    if left:
        errs.append(f"placeholders left: {sorted(set(left))}")
    m = re.search(r"entryFunction: window\.([A-Za-z0-9_$]+),", raw)
    if not m:
        errs.append("entryFunction: window.<name>, missing")
    if "containerElements: [screen]," not in raw:
        errs.append("containerElements: [screen], missing")
    srcs = [a.get("src", "") for t, a in p.tags if t == "script" and a.get("src")]
    app_js = [s for s in srcs if s.endswith(".js") and s != "qtloader.js"]
    if "qtloader.js" not in srcs or len(app_js) != 1:
        errs.append(f"script src = {srcs}")
    elif m:
        js_path = path.parent / app_js[0]
        if js_path.is_file():
            js = js_path.read_text(encoding="utf-8", errors="replace")
            if not re.search(r"\b" + re.escape(m.group(1)) + r"\b", js):
                errs.append(f"{app_js[0]} does not define {m.group(1)}")
            else:
                print(f"  {app_js[0]} defines {m.group(1)}")
    for bad in ("qtlogo", "Loading...", "Qt for WebAssembly", "Application exit"):
        if bad in raw:
            errs.append(f"Qt default text present: {bad!r}")

    if any(t == "link" for t, _ in p.tags):
        errs.append("<link> element present")
    for t, a in p.tags:
        for attr in ("src", "href"):
            v = a.get(attr, "")
            if re.match(r"^(?:[a-z]+:)?//", v, re.I):
                errs.append(f"external {attr}: {v}")
    css = "\n".join(p.styles)
    if re.search(r"@import|url\(", css):
        errs.append("CSS @import / url() present")
    if re.search(r"https?://", raw):
        errs.append("http(s):// URL present")
    errs += css_check(css)
    for want in DESIGN:
        if want not in css:
            errs.append(f"design value missing in CSS: {want!r}")
    for want in TEXTS:
        if want not in raw:
            errs.append(f"text missing: {want!r}")

    inline = [s for s in p.scripts if s.strip()]
    if len(inline) != 1:
        errs.append(f"inline scripts = {len(inline)}")
    elif node:
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "inline.js"
            f.write_text(inline[0], encoding="utf-8")
            r = subprocess.run([node, "--check", str(f)], capture_output=True, text=True)
            if r.returncode != 0:
                errs.append(f"node --check failed: {r.stderr.strip()[:400]}")
            else:
                print(f"  node --check inline script: OK")
    else:
        print("  node not found: JS syntax not checked")
    print(f"  elements={len(p.tags)} style_bytes={len(css)} script_bytes={len(inline[0]) if inline else 0}")
    return errs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pages", nargs="+")
    ap.add_argument("--node", default=r"C:\tools\emsdk\node\16.20.0_64bit\bin\node.exe")
    args = ap.parse_args()
    node = args.node if Path(args.node).is_file() else None
    bad = 0
    for page in args.pages:
        path = Path(page)
        print(f"{path}:")
        errs = check(path, node)
        for e in errs:
            print(f"  FAIL {e}")
        print(f"  {'OK' if not errs else 'FAILED'} ({len(errs)} problem(s))")
        bad += bool(errs)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
