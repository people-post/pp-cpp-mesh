#!/usr/bin/env bash
# Guard include edges between the pp-cpp-mesh folder libraries, and what the library may include.
#
# The allowed edges are the DEPS of each library in src/CMakeLists.txt (transitively), so the link
# graph and the include graph cannot drift apart: add the DEPS entry, or do not add the include.
# Folders map to libraries as: shared/ and l4/shared/ -> shared, l4/<proto>/ -> <proto>,
# <folder>/ -> <folder>. Every library but types sits on types. Tests may include anything here.
#
# Quoted includes name this library (mesh/) or its dependencies (common/ = pp-cpp-common,
# crypto/ = pp-cpp-crypto, amp/ = pp-cpp-amp) — never an application header.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
cmake = (root / "src" / "CMakeLists.txt").read_text()

deps = {}
for m in re.finditer(r"(_pp_mesh_library|pp_mesh_add_library)\(\s*(\w+)(.*?)\)", cmake, re.S):
    kind, name, body = m.group(1), m.group(2), m.group(3)
    d = re.search(r"DEPS\s+([\w\s]+?)(?:\s+(?:LIBS|SOURCES)\b|$)", body.strip(), re.S)
    deps[name] = set(d.group(1).split()) if d else set()
    if kind == "_pp_mesh_library":
        deps[name].add("types")

def closure(name, seen=None):
    seen = set() if seen is None else seen
    for dep in deps.get(name, ()):
        if dep not in seen:
            seen.add(dep)
            closure(dep, seen)
    return seen

allowed = {name: closure(name) | {name} for name in deps}

def lib_of(rel):
    parts = rel.split("/")
    if parts[0] == "shared" or parts[:2] == ["l4", "shared"]:
        return "shared"
    if parts[0] == "l4":
        return parts[1]
    return parts[0]

EXTERNAL = ("common/", "crypto/", "amp/")
fail = 0
for base in (root / "include" / "mesh", root / "src", root / "tests"):
    for path in sorted(base.rglob("*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        rel = path.relative_to(base).as_posix()
        is_test = base.name == "tests"
        src = None if is_test else lib_of(rel)
        if src is not None and src not in allowed:
            print(f"FAIL: {path.relative_to(root)}: folder has no library ({src})")
            fail = 1
            continue
        for n, line in enumerate(path.read_text().splitlines(), 1):
            m = re.match(r'\s*#include "([^"]+)"', line)
            if not m:
                continue
            inc = m.group(1)
            if inc.startswith("mesh/"):
                dst = lib_of(inc[len("mesh/"):])
                if src is not None and dst not in allowed[src]:
                    print(f"FAIL: {path.relative_to(root)}:{n}: {src} -> {dst} is not a DEPS edge")
                    fail = 1
            elif not inc.startswith(EXTERNAL):
                print(f"FAIL: {path.relative_to(root)}:{n}: {inc} is neither mesh/ nor a dependency's")
                fail = 1
sys.exit(fail)
PY

echo "OK: pp-cpp-mesh include edges"
