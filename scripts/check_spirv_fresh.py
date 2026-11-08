#!/usr/bin/env python3
"""Fails when a Vulkan shader's inputs changed but its committed .spv did not.

A shader counts as edited when its own source or any file it #includes
(glslc -M) is in --changed. An edited shader passes if its .spv is also in
--changed, or if it compiles to the same SPIR-V at --base and --head, so
comment-only edits need no new binary. A shader glslc cannot compile at
either tree is reported on its own, with glslc's messages.
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


class GlslcError(Exception):
    """glslc failed on a shader in one tree; carries what it printed."""

    def __init__(self, src, tree, output):
        super().__init__(f"cannot compile {src} at {tree}")
        self.src = src
        self.tree = tree
        self.output = output.strip()


def run_glslc(cmd, src, tree):
    """Runs glslc; stdout, or GlslcError with its messages on failure."""
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise GlslcError(src, tree, result.stderr or result.stdout)
    return result.stdout


def dependencies(glslc, root, includes, src, tree):
    """Tree-relative paths of the source and everything it includes."""
    out = run_glslc([glslc, "-M"] + include_flags(root, includes) +
                    [str(root / src)], src, tree)
    deps = out.split(":", 1)[1].replace("\\\n", " ").split()
    real_root = os.path.realpath(root)
    return {os.path.relpath(os.path.realpath(d), real_root) for d in deps}


def compile_spirv(glslc, root, includes, src, out, tree):
    """Compiles one shader to out."""
    run_glslc([glslc] + include_flags(root, includes) +
              [str(root / src), "-o", str(out)], src, tree)


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
    broken = []
    with tempfile.TemporaryDirectory() as tmp:
        base_spv = pathlib.Path(tmp) / "base.spv"
        head_spv = pathlib.Path(tmp) / "head.spv"
        for path in sources:
            src = f"{args.shaders}/{path.name}"
            if f"{src}.spv" in changed:
                continue
            try:
                deps = dependencies(args.glslc, args.head, args.include, src,
                                    "head")
                if not deps & changed:
                    continue
                if not (args.base / src).is_file():
                    stale.append(src)
                    continue
                compile_spirv(args.glslc, args.head, args.include, src,
                              head_spv, "head")
                compile_spirv(args.glslc, args.base, args.include, src,
                              base_spv, "base")
            except GlslcError as error:
                broken.append(error)
                continue
            if base_spv.read_bytes() != head_spv.read_bytes():
                stale.append(src)
    for error in broken:
        hint = "fix it" if error.tree == "head" else (
            f"so it cannot be compared, commit a fresh {error.src}.spv")
        print(f"{error} ({hint}):\n{error.output}")
    for src in stale:
        print(f"{src} or an include it uses changed but {src}.spv did not. "
              f"Recompile it with glslc -I <include dirs> (SleakCraft: "
              f"scripts/compile_shaders.sh) and commit the .spv")
    return 1 if stale or broken else 0


if __name__ == "__main__":
    sys.exit(main())
