#!/usr/bin/env python3
"""Compiles every GLSL shader in plugin/src with glslangValidator.

The shaders only compile when Resolume loads the plugin, so a typo, a reserved word or a uniform
that hides a built-in shows up as a black output rather than a build failure. This catches those
at build time instead. Skips quietly when glslangValidator is not installed.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"


def raw_strings(text):
    """Every C++ raw string literal in a source file, with the name it is assigned to."""
    for match in re.finditer(r'(\w+)(?:\[\s*\])?\s*=\s*R"\((.*?)\)";', text, re.S):
        yield match.group(1), match.group(2)
    # CompositionVertexHeader and friends return their GLSL rather than assigning it.
    for match in re.finditer(r'(\w+)\(\)\s*\{\s*return R"\((.*?)\)";', text, re.S):
        yield match.group(1), match.group(2)


def shared_glsl():
    """The GLSL helpers the plugins paste in front of their own shaders."""
    common = (SRC / "PluginCommon.h").read_text()
    return {name: body for name, body in raw_strings(common)}


def jobs():
    """Each shader to check: a label, the GLSL, and the stage it belongs to."""
    shared = shared_glsl()
    header = shared.get("CompositionVertexHeader", "")
    wipe = shared.get("WipeAlphaGLSL", "")
    for path in sorted(SRC.glob("*.cpp")):
        for name, body in raw_strings(path.read_text()):
            if "void main" not in body and "wipeAlpha" not in body:
                continue
            stage = "vert" if "gl_Position" in body else "frag"
            glsl = body
            if not glsl.lstrip().startswith("#version"):
                # The plugin builds these by hand; match what it does at runtime.
                if stage == "vert":
                    glsl = header + glsl
                else:
                    glsl = "#version 410 core\n" + wipe + glsl
            if not glsl.lstrip().startswith("#version"):
                continue
            yield f"{path.name}:{name}", glsl, stage


def main():
    validator = shutil.which("glslangValidator")
    if not validator:
        print("glslangValidator not found, skipping shader checks")
        return 0

    failures = 0
    checked = 0
    with tempfile.TemporaryDirectory() as tmp:
        for label, glsl, stage in jobs():
            path = Path(tmp) / f"shader.{stage}"
            path.write_text(glsl)
            result = subprocess.run([validator, str(path)], capture_output=True, text=True)
            checked += 1
            if result.returncode:
                failures += 1
                print(f"FAIL {label}")
                for line in result.stdout.splitlines()[1:]:
                    if line.strip():
                        print(f"     {line}")
            else:
                print(f"ok   {label}")

    print(f"\n{checked} shader(s) checked, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
