#!/usr/bin/env python3
"""Fails when a Vulkan shader's inputs changed but its committed .spv did not.

A shader counts as edited when its own source or any file it #includes
(glslc -M) is in --changed. An edited shader passes if its .spv is also in
--changed, or if it compiles to the same SPIR-V at --base and --head, so
comment-only edits need no new binary.
"""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


def include_flags(root, includes):
    """glslc -I flags for the include folders under a tree."""
    flags = []
    for inc in includes:
        flags += ["-I", str(root / inc)]
    return flags


def dependencies(glslc, root, includes, src):
    """Tree-relative paths of the source and everything it includes."""
    out = subprocess.run(
        [glslc, "-M"] + include_flags(root, includes) + [str(root / src)],
        check=True, capture_output=True, text=True).stdout
    deps = out.split(":", 1)[1].replace("\\\n", " ").split()
    real_root = os.path.realpath(root)
    return {os.path.relpath(os.path.realpath(d), real_root) for d in deps}


def compile_spirv(glslc, root, includes, src, out):
    """Compiles one shader; False when it does not compile."""
    result = subprocess.run(
        [glslc] + include_flags(root, includes) + [str(root / src), "-o",
                                                   str(out)],
        capture_output=True, text=True)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True, type=pathlib.Path)
    parser.add_argument("--head", required=True, type=pathlib.Path)
    parser.add_argument("--shaders", required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--changed", required=True)
    parser.add_argument("--glslc", default="glslc")
    args = parser.parse_args()

    changed = set(pathlib.Path(args.changed).read_text().split())
    shader_dir = args.head / args.shaders
    sources = sorted(p for ext in ("*.vert", "*.frag")
                     for p in shader_dir.glob(ext)
                     if not p.stem.endswith("_gl"))
    stale = []
    with tempfile.TemporaryDirectory() as tmp:
        for path in sources:
            src = f"{args.shaders}/{path.name}"
            if f"{src}.spv" in changed:
                continue
            deps = dependencies(args.glslc, args.head, args.include, src)
            if not deps & changed:
                continue
            base_spv = pathlib.Path(tmp) / "base.spv"
            head_spv = pathlib.Path(tmp) / "head.spv"
            same = ((args.base / src).is_file() and
                    compile_spirv(args.glslc, args.base, args.include, src,
                                  base_spv) and
                    compile_spirv(args.glslc, args.head, args.include, src,
                                  head_spv) and
                    base_spv.read_bytes() == head_spv.read_bytes())
            if not same:
                stale.append(src)
    for src in stale:
        print(f"{src} or an include it uses changed but {src}.spv did not. "
              f"Recompile it with glslc -I <include dirs> (SleakCraft: "
              f"scripts/compile_shaders.sh) and commit the .spv")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
