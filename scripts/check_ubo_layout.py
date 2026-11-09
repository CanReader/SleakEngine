#!/usr/bin/env python3
"""Checks GLSL uniform blocks against the C++ structs that fill them.

Each --check STRUCT:INCLUDE:BLOCK compiles a probe that includes INCLUDE,
reads BLOCK's member offsets from spirv-dis and compares them, in order,
with the static_assert(offsetof(STRUCT, Field) == N) lines in --header.
C++ padding fields (leading underscore) without a GLSL twin are skipped.

Each --constant NAME:INCLUDE:MACRO compares the C++ constant NAME with the
#define MACRO in INCLUDE. Each --flags PREFIX:INCLUDE:MACRO_PREFIX pairs every
C++ enumerator named PREFIX* with the #define MACRO_PREFIX* of the same name
(case and underscores ignored) and compares their values.
"""

import argparse
import ast
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


def int_value(expr):
    """Value of integer literals joined by << and |, or None."""
    text = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uU]?\b", r"\1", expr.strip())
    try:
        tree = ast.parse(text, mode="eval").body
    except SyntaxError:
        return None

    def value(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(
                node.op, (ast.LShift, ast.BitOr)):
            left, right = value(node.left), value(node.right)
            if left is None or right is None:
                return None
            if isinstance(node.op, ast.LShift):
                return left << right
            return left | right
        return None

    return value(tree)


def read_include(include_dirs, include):
    """Text of an include file from the first folder that has it."""
    for d in include_dirs:
        path = pathlib.Path(d) / include
        if path.is_file():
            return path.read_text()
    return None


def glsl_macros(text, prefix):
    """Values of the #defines whose names start with prefix."""
    pattern = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(" + re.escape(prefix) +
                         r"\w*)[ \t]+([^/\n]+)", re.MULTILINE)
    return {m.group(1): int_value(m.group(2)) for m in pattern.finditer(text)}


def cpp_constants(header, prefix):
    """Values of the C++ constants and enumerators named prefix*."""
    pattern = re.compile(r"\b(" + re.escape(prefix) +
                         r"\w*)\s*=\s*([^;,}]+)")
    return {m.group(1): int_value(m.group(2))
            for m in pattern.finditer(header)}


def compare_constant(header, include_dirs, spec):
    """Compares one C++ constant with its GLSL #define; True when equal."""
    name, include, macro = spec.split(":")
    text = read_include(include_dirs, include)
    if text is None:
        print(f"{macro}: {include} not found")
        return False
    cpp = cpp_constants(header, name).get(name)
    glsl = glsl_macros(text, macro).get(macro)
    if cpp is None or glsl is None:
        print(f"{name} / {macro}: missing or not a plain integer "
              f"(C++ {cpp}, GLSL {glsl})")
        return False
    if cpp != glsl:
        print(f"{name} is {cpp} but {macro} is {glsl}, bump both together")
        return False
    print(f"{name} matches {macro} ({cpp})")
    return True


def compare_flags(header, include_dirs, spec):
    """Pairs C++ and GLSL flags by name; True when every value matches."""
    prefix, include, macro_prefix = spec.split(":")
    text = read_include(include_dirs, include)
    if text is None:
        print(f"{macro_prefix}*: {include} not found")
        return False

    def key(name, start):
        return name[len(start):].replace("_", "").upper()

    cpp = {key(n, prefix): (n, v)
           for n, v in cpp_constants(header, prefix).items()}
    glsl = {key(n, macro_prefix): (n, v)
            for n, v in glsl_macros(text, macro_prefix).items()}
    ok = bool(cpp)
    if not cpp:
        print(f"{prefix}*: no C++ flags found")
    for k in sorted(cpp.keys() | glsl.keys()):
        if k not in glsl:
            print(f"{cpp[k][0]} has no {macro_prefix}* twin in {include}")
            ok = False
        elif k not in cpp:
            print(f"{glsl[k][0]} has no {prefix}* twin in the header")
            ok = False
        elif cpp[k][1] is None or cpp[k][1] != glsl[k][1]:
            print(f"{cpp[k][0]} is {cpp[k][1]} but {glsl[k][0]} is "
                  f"{glsl[k][1]}")
            ok = False
    if ok:
        print(f"{prefix}* matches {macro_prefix}* ({len(cpp)} flags)")
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", required=True)
    parser.add_argument("--include-dir", action="append", required=True)
    parser.add_argument("--check", action="append", default=[])
    parser.add_argument("--constant", action="append", default=[])
    parser.add_argument("--flags", action="append", default=[])
    parser.add_argument("--glslc", default="glslc")
    args = parser.parse_args()
    if not (args.check or args.constant or args.flags):
        parser.error("nothing to check")

    header = pathlib.Path(args.header).read_text()
    failed = False
    for check in args.check:
        struct, include, block = check.split(":")
        if not compare(struct, cpp_offsets(header, struct), block,
                       glsl_offsets(args.glslc, args.include_dir, include,
                                    block)):
            failed = True
    for spec in args.constant:
        if not compare_constant(header, args.include_dir, spec):
            failed = True
    for spec in args.flags:
        if not compare_flags(header, args.include_dir, spec):
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
