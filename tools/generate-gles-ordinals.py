#!/usr/bin/env python3
"""Generate ordinal -> name tables for the OpenGL ES 1.x client DLLs.

Windows Mobile games link OpenGL ES through one of two Khronos-defined
client libraries:

* ``libGLES_CM.dll`` — the *Common* profile: every entry point exists in
  both a floating-point (``glFogf``) and a fixed-point (``glFogx``)
  flavour.
* ``libGLES_CL.dll`` — the *Common-Lite* profile: fixed-point only, so
  the ``*f`` entry points are absent.

Both are exported in a single alphabetically-sorted block: the EGL 1.0
entry points first, then the GL ES 1.1 entry points, each ordered by
name, with the ordinal base at 1. That layout is what a plain
``.def``-less MSVC link produces from an alphabetised export list, and
it is what shipping vendor DLLs use.

Only the *names and their ordering* are encoded here — no vendor code is
copied or disassembled. The tables are verified against the import
directory of a real game binary by ``tests`` in ``pocket-gles``.

Usage:
    python3 tools/generate-gles-ordinals.py --out crates/pocket-gles/data
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

# --- EGL 1.0 -------------------------------------------------------------
# The complete EGL 1.0 entry-point set, alphabetically.
EGL_10 = [
    "eglChooseConfig",
    "eglCopyBuffers",
    "eglCreateContext",
    "eglCreatePbufferSurface",
    "eglCreatePixmapSurface",
    "eglCreateWindowSurface",
    "eglDestroyContext",
    "eglDestroySurface",
    "eglGetConfigAttrib",
    "eglGetConfigs",
    "eglGetCurrentContext",
    "eglGetCurrentDisplay",
    "eglGetCurrentSurface",
    "eglGetDisplay",
    "eglGetError",
    "eglGetProcAddress",
    "eglInitialize",
    "eglMakeCurrent",
    "eglQueryContext",
    "eglQueryString",
    "eglQuerySurface",
    "eglSwapBuffers",
    "eglTerminate",
    "eglWaitGL",
    "eglWaitNative",
]

# EGL 1.1 adds surface/texture binding and swap interval; EGL 1.2 adds
# the client-API selectors. A Common-profile DLL from the WM6 era ships
# EGL 1.2, so these follow the 1.0 block in the same alphabetical run.
EGL_11_12_EXTRA = [
    "eglBindAPI",
    "eglBindTexImage",
    "eglCreatePbufferFromClientBuffer",
    "eglQueryAPI",
    "eglReleaseTexImage",
    "eglReleaseThread",
    "eglSurfaceAttrib",
    "eglSwapInterval",
    "eglWaitClient",
]

# --- GL ES 1.1 -----------------------------------------------------------
# Entry points common to both profiles (integer / fixed-point / untyped).
GLES_11_SHARED = [
    "glActiveTexture", "glAlphaFuncx", "glBindTexture", "glBlendFunc",
    "glClear", "glClearColorx", "glClearDepthx", "glClearStencil",
    "glClientActiveTexture", "glColor4x", "glColorMask", "glColorPointer",
    "glCompressedTexImage2D", "glCompressedTexSubImage2D",
    "glCopyTexImage2D", "glCopyTexSubImage2D", "glCullFace",
    "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDepthRangex",
    "glDisable", "glDisableClientState", "glDrawArrays", "glDrawElements",
    "glEnable", "glEnableClientState", "glFinish", "glFlush", "glFogx",
    "glFogxv", "glFrontFace", "glFrustumx", "glGenTextures", "glGetError",
    "glGetIntegerv", "glGetString", "glHint", "glLightModelx",
    "glLightModelxv", "glLightx", "glLightxv", "glLineWidthx",
    "glLoadIdentity", "glLoadMatrixx", "glLogicOp", "glMaterialx",
    "glMaterialxv", "glMatrixMode", "glMultMatrixx", "glMultiTexCoord4x",
    "glNormal3x", "glNormalPointer", "glOrthox", "glPixelStorei",
    "glPointSizex", "glPolygonOffsetx", "glPopMatrix", "glPushMatrix",
    "glReadPixels", "glRotatex", "glSampleCoveragex", "glScalex",
    "glScissor", "glShadeModel", "glStencilFunc", "glStencilMask",
    "glStencilOp", "glTexCoordPointer", "glTexEnvx", "glTexEnvxv",
    "glTexImage2D", "glTexParameterx", "glTexSubImage2D", "glTranslatex",
    "glVertexPointer", "glViewport",
]

# Floating-point entry points, present only in the Common profile.
GLES_11_FLOAT_ONLY = [
    "glAlphaFunc", "glClearColor", "glClearDepthf", "glColor4f",
    "glDepthRangef", "glFogf", "glFogfv", "glFrustumf", "glLightModelf",
    "glLightModelfv", "glLightf", "glLightfv", "glLineWidth",
    "glLoadMatrixf", "glMaterialf", "glMaterialfv", "glMultMatrixf",
    "glMultiTexCoord4f", "glNormal3f", "glOrthof", "glPointSize",
    "glPolygonOffset", "glRotatef", "glSampleCoverage", "glScalef",
    "glTexEnvf", "glTexEnvfv", "glTexParameterf", "glTranslatef",
]


def build(profile: str) -> dict[str, str]:
    """Build the ordinal -> name map for ``cm`` or ``cl``."""
    if profile == "cm":
        egl = sorted(EGL_10 + EGL_11_12_EXTRA)
        gl = sorted(GLES_11_SHARED + GLES_11_FLOAT_ONLY)
    elif profile == "cl":
        egl = sorted(EGL_10)
        gl = sorted(GLES_11_SHARED)
    else:
        raise ValueError(profile)
    names = egl + gl
    return {str(i): n for i, n in enumerate(names, start=1)}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    for profile, dll in (("cm", "libGLES_CM.dll"), ("cl", "libGLES_CL.dll")):
        table = build(profile)
        path = args.out / f"libgles_{profile}-ordinals.json"
        payload = {"dll": dll, "ordinals": table}
        path.write_text(json.dumps(payload, indent=2, sort_keys=False) + "\n")
        print(f"wrote {path} ({len(table)} ordinals)")


if __name__ == "__main__":
    main()
