"""w2-043 D3: check that the rewritten `scope` lines in scripts/serve_wasm.py keep the banner
behaviour (run with `python -B` so no __pycache__ is written into the repo).

The real serve_wasm.main() runs with the real argument parsing and file checks; only the
socket server class is replaced by a stand-in whose serve_forever() returns at once
(KeyboardInterrupt), so no port is bound (in particular no 0.0.0.0 bind on this machine).
Expected banner suffix:
  --host 0.0.0.0 (default) / --host ""  -> " (all interfaces: also http://<LAN IP>:<port>/TaidaFlowApp.html)"
  --host 127.0.0.1                       -> no suffix
Exit 0 = all three cases as expected.
"""
import contextlib
import io
import sys
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[4] / "scripts"
sys.dont_write_bytecode = True
sys.path.insert(0, str(SCRIPTS))
import serve_wasm  # noqa: E402


class _NoBindServer:
    def __init__(self, address, handler):
        self.address = address

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def serve_forever(self):
        raise KeyboardInterrupt


def banner(host_args):
    sys.argv = ["serve_wasm.py", "--port", "8123", *host_args]
    buf = io.StringIO()
    orig = serve_wasm.http.server.ThreadingHTTPServer
    serve_wasm.http.server.ThreadingHTTPServer = _NoBindServer
    try:
        with contextlib.redirect_stdout(buf):
            rc = serve_wasm.main()
    finally:
        serve_wasm.http.server.ThreadingHTTPServer = orig
    return rc, buf.getvalue().strip()


suffix = " (all interfaces: also http://<LAN IP>:8123/TaidaFlowApp.html)"
cases = [([], True), (["--host", ""], True), (["--host", "127.0.0.1"], False)]
ok = True
for args, want_suffix in cases:
    rc, line = banner(args)
    good = rc == 0 and line.endswith("/TaidaFlowApp.html" + (suffix if want_suffix else ""))
    ok &= good
    print(f"{'OK ' if good else 'BAD'} args={args!r} rc={rc} banner={line!r}")
print("RESULT", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
