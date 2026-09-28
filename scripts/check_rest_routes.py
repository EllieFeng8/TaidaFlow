#!/usr/bin/env python3
"""Compare the REST route table that Core logs with the routes RESTManager really registers (w2-060).

  python -B scripts/check_rest_routes.py

* Core/RESTManager.cpp : every  m_httpServer.route("<path>", QHttpServerRequest::Method::<M>, ...)
* Core/core.cpp        : the kRestRoutes table ({"GET,PUT,OPTIONS", "<path>", "<purpose>"}) that
                         Core::startRestServer() prints as "[REST] route ..." at start-up and that
                         README / DEPLOY_AND_STARTUP.md document.

Prints both sets as "<METHOD> <path>" and the differences. A route registered twice (same method
and path) is reported as a note: QHttpServer uses the first registration, the second is never
reached. Exit 0 when the (method, path) sets are equal, 1 when they differ, 2 on a parse error.
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    rest = (ROOT / "Core" / "RESTManager.cpp").read_text(encoding="utf-8")
    core = (ROOT / "Core" / "core.cpp").read_text(encoding="utf-8")

    registered = re.findall(
        r'm_httpServer\.route\(\s*"([^"]+)"\s*,\s*QHttpServerRequest::Method::(\w+)', rest)
    if not registered:
        print("no m_httpServer.route(...) calls found in Core/RESTManager.cpp")
        return 2
    code = Counter((m.upper(), p) for p, m in registered)

    table = re.search(r"constexpr RestRoute kRestRoutes\[\] = \{(.*?)\n\};", core, re.S)
    if not table:
        print("kRestRoutes table not found in Core/core.cpp")
        return 2
    logged = set()
    for methods, path in re.findall(r'\{\s*"([A-Z,]+)"\s*,\s*"([^"]+)"\s*,', table.group(1)):
        for m in methods.split(","):
            logged.add((m, path))

    print(f"RESTManager.cpp registrations: {sum(code.values())} ({len(code)} distinct method+path)")
    for (m, p) in sorted(code, key=lambda k: (k[1], k[0])):
        print(f"  {m:<8} {p}")
    for key, n in sorted(code.items()):
        if n > 1:
            print(f"  note: {key[0]} {key[1]} is registered {n} times (the first registration answers)")
    print(f"core.cpp kRestRoutes: {len(logged)} method+path")
    only_code = sorted(set(code) - logged)
    only_table = sorted(logged - set(code))
    for m, p in only_code:
        print(f"  MISSING in kRestRoutes: {m} {p}")
    for m, p in only_table:
        print(f"  NOT registered by RESTManager: {m} {p}")
    gets = sorted(p for (m, p) in code if m == "GET")
    puts = sorted(p for (m, p) in code if m == "PUT")
    print(f"GET routes ({len(gets)}): {' '.join(gets)}")
    print(f"PUT routes ({len(puts)}): {' '.join(puts)}")
    ok = not only_code and not only_table
    print(f"check_rest_routes exit={0 if ok else 1}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
