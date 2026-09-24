#!/usr/bin/env python3
"""Development static HTTP server for the TaidaFlow WebAssembly build.

Serves build/wasm-release (or --dir) on http://127.0.0.1:8123/ as required by the
wasm-mirror pack (integration-pack/wasm-mirror/docs/http-server-requirements.md):
  * the page Origin http://127.0.0.1:8123 is in the desktop mirror allowedOrigins
  * correct MIME types (.wasm -> application/wasm, .js -> text/javascript)
  * Cache-Control: no-store so HTML/JS/WASM from different builds never mix
  * Cross-Origin-Opener-Policy: same-origin + Cross-Origin-Embedder-Policy: require-corp
    (not needed by wasm_singlethread, but harmless and makes the same server usable
    for a wasm_multithread build, which needs cross-origin isolation)
The mirror WebSocket (ws://127.0.0.1:8125/mirror) is served by the desktop app, not here.

Usage:  python scripts/serve_wasm.py [--dir build/wasm-release] [--port 8123]
Open:   http://127.0.0.1:8123/TaidaFlowApp.html

Evidence capture (optional, off by default): with --evidence-dir DIR the server also
accepts `PUT /__evidence/<name>.png|.txt` from the page (same origin) and stores the body
in DIR. Used only to save browser-side screenshots/console logs for the E2E report.
"""
from __future__ import annotations

import argparse
import functools
import http.server
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVIDENCE_NAME = re.compile(r"^/__evidence/([A-Za-z0-9._-]{1,100}\.(?:png|txt|json))$")


# Injected into the HTML *only* in --evidence-dir mode: keeps the WebGL back buffer
# readable (preserveDrawingBuffer) so the page can be saved as PNG, and records console
# output with timestamps so it can be uploaded as a log. The app itself is untouched.
EVIDENCE_SHIM = b"""
    <script>/* evidence-capture shim (serve_wasm.py --evidence-dir only) */
      (function () {
        const orig = HTMLCanvasElement.prototype.getContext;
        HTMLCanvasElement.prototype.getContext = function (type, attrs) {
          if (type === 'webgl' || type === 'webgl2' || type === 'experimental-webgl')
            attrs = Object.assign({}, attrs || {}, { preserveDrawingBuffer: true });
          return orig.call(this, type, attrs);
        };
        window.__evidenceLog = [];
        for (const level of ['log', 'info', 'warn', 'error', 'debug']) {
          const o = console[level].bind(console);
          console[level] = function (...args) {
            window.__evidenceLog.push(new Date().toISOString() + ' [' + level + '] ' + args.join(' '));
            o(...args);
          };
        }
      })();
    </script>
"""


class Handler(http.server.SimpleHTTPRequestHandler):
    evidence_dir: Path | None = None

    def do_GET(self) -> None:  # noqa: N802
        if self.evidence_dir is not None and self.path.split("?")[0].endswith(".html"):
            target = Path(self.directory) / self.path.split("?")[0].lstrip("/")
            if target.is_file():
                body = target.read_bytes().replace(b"</title>", b"</title>" + EVIDENCE_SHIM, 1)
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
        super().do_GET()

    def do_PUT(self) -> None:  # noqa: N802 (http.server naming)
        m = EVIDENCE_NAME.match(self.path)
        if self.evidence_dir is None or not m:
            self.send_error(403, "evidence upload disabled or bad name")
            return
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 20 * 1024 * 1024:
            self.send_error(400, "bad length")
            return
        data = self.rfile.read(length)
        self.evidence_dir.mkdir(parents=True, exist_ok=True)
        (self.evidence_dir / m.group(1)).write_bytes(data)
        self.send_response(201)
        self.send_header("Content-Length", "0")
        self.end_headers()

    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".html": "text/html; charset=utf-8",
        ".svg": "image/svg+xml",
    }

    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, default=ROOT / "build" / "wasm-release")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--evidence-dir", type=Path, default=None,
                    help="enable PUT /__evidence/<name> uploads into this directory")
    args = ap.parse_args()
    if args.evidence_dir is not None:
        Handler.evidence_dir = args.evidence_dir.resolve()
        print(f"evidence uploads enabled -> {Handler.evidence_dir}", flush=True)

    root = args.dir.resolve()
    for required in ("TaidaFlowApp.html", "TaidaFlowApp.js", "TaidaFlowApp.wasm", "qtloader.js"):
        if not (root / required).is_file():
            print(f"missing {root / required} - build first: scripts\\build-wasm.bat", file=sys.stderr)
            return 2

    handler = functools.partial(Handler, directory=str(root))
    with http.server.ThreadingHTTPServer((args.host, args.port), handler) as httpd:
        print(f"serving {root} at http://{args.host}:{args.port}/TaidaFlowApp.html", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
