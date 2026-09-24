#!/usr/bin/env python3
"""Verify the vendored wasm-mirror pack (integration-pack/wasm-mirror).

  python scripts/verify_pack.py [--source <pack release dir>]

1. MANIFEST.sha256 check: every listed file exists and its SHA-256 matches
   (prints ok/total, e.g. 24/24), and no file in the pack is missing from the manifest
   (MANIFEST.sha256 itself excepted).
2. With --source: the pack directory and the release source contain exactly the same
   file set and every file is byte-identical (SHA-256 per file, each pair printed).
3. The legacy 1.0.0 extensionless `VERSION` file must not exist; VERSION.txt is reported.

Exit 0 only when every check passes. Read-only: nothing is written.
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "integration-pack" / "wasm-mirror"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def files_of(base: Path) -> dict[str, Path]:
    return {p.relative_to(base).as_posix(): p for p in sorted(base.rglob("*")) if p.is_file()}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, help="pack release directory to compare against")
    args = ap.parse_args()
    status = 0

    manifest = PACK / "MANIFEST.sha256"
    entries = []
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if line.strip():
            digest, name = line.split(None, 1)
            entries.append((digest.lower(), name.strip()))
    ok = 0
    for digest, name in entries:
        path = PACK / name
        if not path.is_file():
            print(f"MANIFEST missing file: {name}")
            continue
        actual = sha256(path)
        if actual == digest:
            ok += 1
        else:
            print(f"MANIFEST mismatch: {name} expected={digest} actual={actual}")
    print(f"manifest: {ok}/{len(entries)} ok")
    if ok != len(entries) or not entries:
        status = 1
    listed = {name for _, name in entries} | {"MANIFEST.sha256"}
    unlisted = sorted(set(files_of(PACK)) - listed)
    for name in unlisted:
        print(f"file not in MANIFEST: {name}")
    if unlisted:
        status = 1

    names = set(files_of(PACK))
    version_txt = (PACK / "VERSION.txt").read_text(encoding="utf-8").splitlines()[0] \
        if (PACK / "VERSION.txt").is_file() else "<missing>"
    print(f"legacy_VERSION_present={'VERSION' in names} VERSION.txt={version_txt}")
    if "VERSION" in names or version_txt == "<missing>":
        status = 1

    if args.source:
        src = files_of(args.source)
        dst = files_of(PACK)
        only_src = sorted(set(src) - set(dst))
        only_dst = sorted(set(dst) - set(src))
        same = 0
        for name in sorted(set(src) & set(dst)):
            a, b = sha256(src[name]), sha256(dst[name])
            if a == b:
                same += 1
                print(f"  same {a}  {name}")
            else:
                print(f"  DIFF {name} source={a} pack={b}")
        for name in only_src:
            print(f"only in source: {name}")
        for name in only_dst:
            print(f"only in pack: {name}")
        print(f"source compare: identical={same} source_files={len(src)} pack_files={len(dst)} "
              f"only_source={len(only_src)} only_pack={len(only_dst)}")
        if only_src or only_dst or same != len(src):
            status = 1
    print(f"verify_pack exit={status}")
    return status


if __name__ == "__main__":
    sys.exit(main())
