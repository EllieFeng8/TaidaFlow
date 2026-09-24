#!/usr/bin/env python3
"""Prove "Desktop-only Core / WASM only builds the Proxy" on already-built trees.

  python scripts/check_wasm_backend.py build/desktop build/wasm-release

For each build directory (the target is TaidaFlowApp.exe on desktop, TaidaFlowApp.js
on WebAssembly) it reports, from Ninja's own build graph and the produced binary:

  backend_sources   : backend .cpp files (Core/core.cpp, manager.cpp, Modbus_Client.cpp,
                      Modbus_Server.cpp, Ms300FaultReader.cpp, SqlManager.cpp,
                      RESTManager.cpp) that appear in `ninja -t commands <target>`
                      (every compile/link command needed to produce the target)
  backend_qt_libs   : Qt SerialBus / SerialPort / Sql / HttpServer / Concurrent libraries
                      on the final link command
  backend_qt_found  : Qt6<Module>_DIR entries for those modules in CMakeCache.txt
                      (a find_package hit, direct or transitive)
  backend_strings   : backend-only literals found in the binary (.exe / .wasm)

Expected: desktop has all of them (sanity check that the probe works); the WASM build has
backend_sources=0, backend_qt_libs=0, backend_strings=0 and no SerialBus/SerialPort/
HttpServer/Concurrent package. (Qt6Sql_DIR may appear in the WASM cache: Qt's own QML
plugin package scan loads Qt6QmlLocalStorage, which depends on Sql; it is not linked.)

Exit 0 when the WASM tree(s) contain no backend source, library or string and every
desktop tree does contain them; 1 otherwise.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

NINJA = r"C:\Qt\Tools\Ninja\ninja.exe"
BACKEND_SOURCES = ["core.cpp", "manager.cpp", "Modbus_Client.cpp", "Modbus_Server.cpp",
                   "Ms300FaultReader.cpp", "SqlManager.cpp", "RESTManager.cpp"]
BACKEND_MODULES = ["SerialBus", "SerialPort", "Sql", "HttpServer", "Concurrent"]
BACKEND_STRINGS = [b"192.168.1.201", b"192.168.1.205", b"COM2", b"TaidaFlowSettings.ini",
                   b"settings.sqlite", b"device_info.ini", b"[ModbusServer]", b"[MS300]",
                   b"ModbusClient", b"SqlManager", b"RESTManager", b"QModbusTcpClient"]


def analyse(build: Path) -> tuple[bool, str]:
    wasm = (build / "TaidaFlowApp.js").is_file() and (build / "TaidaFlowApp.wasm").is_file()
    target = "TaidaFlowApp.js" if wasm else "TaidaFlowApp.exe"
    binary = build / ("TaidaFlowApp.wasm" if wasm else "TaidaFlowApp.exe")
    out = subprocess.run([NINJA, "-C", str(build), "-t", "commands", target],
                         capture_output=True, text=True, errors="replace", check=False)
    if out.returncode != 0:
        return False, f"{build}: ninja -t commands failed ({out.returncode})"
    commands = out.stdout.splitlines()
    compiled = []
    for src in BACKEND_SOURCES:
        pat = re.compile(r"[/\\]Core[/\\]" + re.escape(src) + r"(\s|\"|$)")
        if any(pat.search(c) for c in commands):
            compiled.append(src)
    # The final link edge in build.ninja: its LINK_LIBRARIES variable holds the libraries
    # (MSVC links through a response file, so `ninja -t commands` only shows @rsp).
    ninja_text = (build / "build.ninja").read_text(encoding="utf-8", errors="replace")
    edge = re.search(r"^build " + re.escape(target) + r"[ :].*?(?=^build |\Z)", ninja_text,
                     re.M | re.S)
    link_vars = edge.group(0) if edge else ""
    libs = [m for m in BACKEND_MODULES
            if re.search(r"(?i)(Qt6" + m + r"\.lib|libQt6" + m + r"\.a)", link_vars)]
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    found = [m for m in BACKEND_MODULES if re.search(r"^Qt6" + m + r"_DIR:PATH=(?!.*NOTFOUND)", cache, re.M)]
    data = binary.read_bytes()
    # QStringLiteral stores UTF-16; plain literals are UTF-8/Latin-1. Look for both.
    strings = [s.decode() for s in BACKEND_STRINGS
               if s in data or s.decode().encode("utf-16-le") in data]
    kind = "wasm" if wasm else "desktop"
    report = (f"{build} [{kind}] target={target} binary_bytes={len(data)} commands={len(commands)}\n"
              f"  backend_sources ({len(compiled)}): {', '.join(compiled) or '-'}\n"
              f"  backend_qt_libs ({len(libs)}): {', '.join(libs) or '-'}\n"
              f"  backend_qt_found ({len(found)}): {', '.join(found) or '-'}\n"
              f"  backend_strings ({len(strings)}): {', '.join(strings) or '-'}")
    if wasm:
        bad_found = [m for m in found if m != "Sql"]
        ok = not compiled and not libs and not strings and not bad_found
    else:
        ok = len(compiled) == len(BACKEND_SOURCES) and bool(libs) and bool(strings)
    return ok, report + f"\n  verdict: {'OK' if ok else 'FAIL'}"


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    status = 0
    for d in argv[1:]:
        ok, report = analyse(Path(d))
        print(report)
        if not ok:
            status = 1
    print(f"check_wasm_backend exit={status}")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
