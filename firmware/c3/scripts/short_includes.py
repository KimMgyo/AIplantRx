"""Keep the C3's compile commands under Windows' 32,767-character limit.

The ESP32-C3 framework adds about 270 include directories (31,146 characters of -I flags, 2026-11-21).
GCC hands every -I on to the assembler too (%{I*} in its default asm spec), so with the few Arduino
libraries the common module needs (WiFi, HTTPClient, ArduinoOTA, ...) the assembler's command line passed
the limit and the build stopped with "cannot execute ... as.exe: CreateProcess: No such file or
directory".

This rewrites only how the include list is spelled: the framework's include root becomes one -iprefix,
and each directory under it an -iwithprefixbefore, which GCC searches exactly where it would search the
-I it replaces and does not pass to the assembler. Everything else stays -I. It works for the libraries'
builds as well because they clone this environment's _CPPINCFLAGS.
"""
import os

Import("env")  # noqa: F821  (SCons)

platform = env.PioPlatform()  # noqa: F821
libs_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
mcu = env.BoardConfig().get("build.mcu", "esp32c3")  # noqa: F821
PREFIX = os.path.join(libs_dir, mcu, "include").replace("\\", "/").rstrip("/") + "/"


def _aipx_inc(paths, env):
    out, moved = [], []
    for p in env.Flatten(paths):
        s = p.get_abspath() if hasattr(p, "get_abspath") else env.subst(str(p))
        s = s.replace("\\", "/")
        if s.startswith(PREFIX):
            moved.append(s[len(PREFIX):])
        else:
            out.append("-I" + s)
    if moved:
        out = ["-iprefix", PREFIX] + out
        for m in moved:
            out += ["-iwithprefixbefore", m]
    return out


env["_aipx_inc"] = _aipx_inc  # noqa: F821
env["_CPPINCFLAGS"] = "${_aipx_inc(CPPPATH, __env__)}"  # noqa: F821
