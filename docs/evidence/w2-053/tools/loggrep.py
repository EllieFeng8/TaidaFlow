#!/usr/bin/env python3
"""w2-053 evidence helper: print lines of a TaidaFlow / simulator stderr log that contain any
of the given substrings, with their line numbers.  Each line is decoded as UTF-8, falling back
to cp950 (the Windows console code page some Qt log lines are written in).

  python -B loggrep.py <log> <needle> [<needle> ...]      (--utf8-out <file>: also write the
                                                            whole log converted to UTF-8)
"""
import sys


def main(argv):
    args = argv[1:]
    out = None
    if "--utf8-out" in args:
        i = args.index("--utf8-out")
        out = args[i + 1]
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 1
    path, needles = args[0], args[1:]
    lines = []
    with open(path, "rb") as f:
        for raw in f.read().split(b"\n"):
            raw = raw.rstrip(b"\r")
            try:
                lines.append(raw.decode("utf-8"))
            except UnicodeDecodeError:
                lines.append(raw.decode("cp950", errors="replace"))
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
    for n, line in enumerate(lines, 1):
        if not needles or any(s in line for s in needles):
            print(f"{n}: {line}")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main(sys.argv))
