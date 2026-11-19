#!/usr/bin/env python3
"""Syntax-check the web assets the firmware serves.

The dashboard's JavaScript and CSS live as raw string literals in
`firmware/src/web_assets.h`. C++ does not look inside them, so a stray brace
compiles fine and ships: the page then parses as nothing, and the owner sees a
blank dashboard that cannot even switch tabs.

    python3 tools/check_web_assets.py

Exit status 1 on the first problem, with the line number counted from the start
of the literal, which is how it appears in the browser console.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "firmware" / "src" / "web_assets.h"

# const char kAppJs[] PROGMEM = R"JS( ... )JS";
# Whitespace between "R" and the opening parenthesis is legal C++ up to a limit,
# and this file relies on it - match it rather than assuming there is none.
RAW = re.compile(r'R"\s*(?P<tag>[A-Z]+)\((?P<body>.*?)\)\s*(?P=tag)"', re.S)


def literals(text: str) -> list[tuple[str, str]]:
    return [(m.group("tag"), m.group("body")) for m in RAW.finditer(text)]


def check_js(body: str) -> list[str]:
    node = shutil.which("node")
    if node is None:
        return ["node is not installed - cannot check the JavaScript"]
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as fh:
        fh.write(body)
        path = fh.name
    result = subprocess.run([node, "--check", path], capture_output=True, text=True)
    Path(path).unlink()
    if result.returncode == 0:
        return []
    return [line for line in result.stderr.splitlines() if line.strip()]


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    found = literals(text)
    if not found:
        print("no raw string literals found in web_assets.h")
        return 1

    problems = 0
    for tag, body in found:
        if tag == "JS":
            errors = check_js(body)
            status = "ok" if not errors else "FAILED"
            print(f"JavaScript ({len(body.splitlines())} lines): {status}")
            for line in errors:
                print("  " + line)
            problems += bool(errors)
        elif tag == "CSS":
            # CSS has no parser here; braces in a style sheet are the mistake that
            # actually happens, and an unbalanced one silently kills the layout.
            opens, closes = body.count("{"), body.count("}")
            ok = opens == closes
            print(f"CSS ({len(body.splitlines())} lines): {'ok' if ok else 'FAILED'}")
            if not ok:
                print(f"  unbalanced braces: {opens} '{{' vs {closes} '}}'")
                problems += 1

    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
