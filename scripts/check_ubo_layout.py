#!/usr/bin/env python3
"""Checks GLSL uniform blocks against the C++ structs that fill them.

Each --check STRUCT:INCLUDE:BLOCK compiles a probe that includes INCLUDE,
reads BLOCK's member offsets from spirv-dis and compares them, in order,
with the static_assert(offsetof(STRUCT, Field) == N) lines in --header.
C++ padding fields (leading underscore) without a GLSL twin are skipped.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

PROBE = """#version 450
#extension GL_GOOGLE_include_directive : require
#include "{include}"
layout(location = 0) out vec4 probeOut;
void main() {{ probeOut = vec4(0.0); }}
"""


def cpp_offsets(header, struct):
    """Fields and offsets from the header's offsetof static_asserts."""
    pattern = re.compile(
        r"static_assert\(\s*offsetof\(\s*" + re.escape(struct) +
        r"\s*,\s*(\w+)\s*\)\s*==\s*(\d+)\s*\)")
    return [(m.group(1), int(m.group(2))) for m in pattern.finditer(header)]


def glsl_offsets(glslc, include_dirs, include, block):
    """Members and offsets of a block as glslc lays it out."""
    with tempfile.TemporaryDirectory() as tmp:
        probe = pathlib.Path(tmp) / "probe.frag"
        spv = pathlib.Path(tmp) / "probe.spv"
        probe.write_text(PROBE.format(include=include))
        cmd = [glslc]
        for d in include_dirs:
            cmd += ["-I", d]
        subprocess.run(cmd + [str(probe), "-o", str(spv)], check=True)
        dis = subprocess.run(["spirv-dis", str(spv)], check=True,
                             capture_output=True, text=True).stdout
    names, offsets = {}, {}
    name_re = re.compile(
        r'OpMemberName %' + re.escape(block) + r' (\d+) "(\w+)"')
    off_re = re.compile(
        r'OpMemberDecorate %' + re.escape(block) + r' (\d+) Offset (\d+)')
    for m in name_re.finditer(dis):
        names[int(m.group(1))] = m.group(2)
    for m in off_re.finditer(dis):
        offsets[int(m.group(1))] = int(m.group(2))
    return [(names.get(i, "?"), offsets[i]) for i in sorted(offsets)]


def compare(struct, cpp, block, glsl):
    """Prints every mismatch; True when the layouts agree."""
    if not cpp or not glsl:
        print(f"{struct}: nothing to compare ({len(cpp)} C++ asserts, "
              f"{len(glsl)} GLSL members)")
        return False
    glsl_names = {name for name, _ in glsl}
    cpp = [(f, o) for f, o in cpp if not f.startswith("_") or f in glsl_names]
    ok = True
    if [o for _, o in cpp] != sorted(o for _, o in cpp):
        print(f"{struct}: static_asserts are not in member order")
        ok = False
    if len(cpp) != len(glsl):
        print(f"{struct}: {len(cpp)} C++ fields but {block} has "
              f"{len(glsl)} members")
        ok = False
    for (field, cpp_off), (member, glsl_off) in zip(cpp, glsl):
        if cpp_off != glsl_off:
            print(f"{struct}.{field} is at {cpp_off} but {block}.{member} "
                  f"is at {glsl_off}")
            ok = False
    if ok:
        print(f"{struct} matches {block} ({len(glsl)} members)")
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", required=True)
    parser.add_argument("--include-dir", action="append", required=True)
    parser.add_argument("--check", action="append", required=True)
    parser.add_argument("--glslc", default="glslc")
    args = parser.parse_args()

    header = pathlib.Path(args.header).read_text()
    failed = False
    for check in args.check:
        struct, include, block = check.split(":")
        if not compare(struct, cpp_offsets(header, struct), block,
                       glsl_offsets(args.glslc, args.include_dir, include,
                                    block)):
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
