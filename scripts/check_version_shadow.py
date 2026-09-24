#!/usr/bin/env python3
"""Guard for the wasm-mirror `VERSION` / `<version>` shadowing landmine.

Pack 1.0.0 shipped a bare file named VERSION at the pack root. With the QDS default
CMAKE_INCLUDE_CURRENT_DIR ON the pack root landed on the include path and, on the
case-insensitive Windows filesystem, `#include <version>` resolved to that file instead
of the C++ standard header. Pack 1.0.1 renamed it to VERSION.txt and turns
CMAKE_INCLUDE_CURRENT_DIR off inside its own directory, so the host needs no workaround.
This script proves it on already-built trees by scanning Ninja's recorded header
dependencies (`ninja -t deps`):

  * version_shadow_hits : object files depending on integration-pack/wasm-mirror/VERSION
                          (must be 0)
  * std_version_deps    : object files whose `version` dependency is some other file
                          (the toolchain's standard header, e.g. MSVC STL / libc++)

  python scripts/check_version_shadow.py build/desktop build/wasm-release

Exit 0 when no object file depends on the pack's VERSION file (and deps exist), 1 otherwise.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

NINJA = r"C:\Qt\Tools\Ninja\ninja.exe"
PATTERN = re.compile(r"integration-pack[/\\]wasm-mirror[/\\]version\s*$", re.IGNORECASE)
ANY_VERSION = re.compile(r"[/\\]version\s*$", re.IGNORECASE)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    status = 0
    for build_dir in argv[1:]:
        out = subprocess.run([NINJA, "-C", build_dir, "-t", "deps"], capture_output=True,
                             text=True, errors="replace", check=False)
        if out.returncode != 0:
            print(f"{build_dir}: ninja -t deps failed ({out.returncode})")
            return 2
        current, hits, objects = "", [], 0
        std_deps: dict[str, int] = {}
        for line in out.stdout.splitlines():
            if line and not line[0].isspace():
                current = line.split(":")[0]
                objects += 1
                continue
            dep = line.strip()
            if PATTERN.search(dep):
                hits.append(current)
            elif ANY_VERSION.search(dep):
                std_deps[dep] = std_deps.get(dep, 0) + 1
        print(f"{Path(build_dir)}: objects_with_deps={objects} version_shadow_hits={len(hits)} "
              f"std_version_deps={sum(std_deps.values())}")
        for h in hits:
            print(f"  shadowed: {h}")
        for dep, count in sorted(std_deps.items()):
            print(f"  <version> -> {dep} ({count} objects)")
        if hits or objects == 0:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
