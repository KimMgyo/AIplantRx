"""Inject this working tree's version into the firmware as FW_VERSION.

WHY THIS EXISTS. The three boards decide whether to install a published image by comparing
esp_app_get_description()->app_elf_sha256 against the one the server computed. A hash answers
"same or different" and nothing else, and it turned out to answer "different" in two situations
that need opposite actions:

  Identical source, different build. Measured on this project's own releases: sensor_node's source
  is byte-identical between v0.4.2 and v0.4.3 (git diff is empty), both images are 1,085,584 bytes,
  and 68 bytes differ - the app descriptor's elf_sha256 and the image's trailing integrity hash.
  So a node that was already running the newest code downloaded a megabyte to install it again.

  A local build against a published one. A USB-flashed image is usually NEWER than what the server
  has, and pressing the panel's update button replaced it - three times, each time losing work that
  had not been released yet.

The descriptor already has a version field and it cannot help: esp_app_desc.version reads "76b7a3f"
in every image this project has ever produced, along with project_name "arduino-lib-builder" and a
build date in February 2025, because those describe the precompiled Arduino core rather than our
code. So the ordering has to come from somewhere we control, which is here.

WHAT IT PRODUCES. `git describe --tags --dirty --always`, which is ordered by construction:

  v0.4.3               an exact tag, which is what CI builds
  v0.4.3-1-g0ab2efa    one commit past it - a local build, and NEWER than the tag
  v0.4.3-1-g0ab2efa-dirty   uncommitted edits on top of that, newer still

shared/fwver.h compares those, and a string it cannot parse is not a version - it makes the caller
fall back to the hash rather than guess an order. That is also the fallback when git is absent or
this is not a repository, which is why failing to describe is a warning and not an error: a tree
someone extracted from a zip still has to build.
"""
Import("env")

import subprocess


def _describe() -> str:
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--dirty", "--always"],
            capture_output=True, text=True, timeout=15,
            cwd=env.subst("$PROJECT_DIR"),
        )
    except (OSError, subprocess.SubprocessError) as exc:
        print("fwver: cannot run git (%s); FW_VERSION will be empty" % exc)
        return ""
    if out.returncode != 0:
        # No tags, a shallow clone that did not fetch them, or not a repository. All three mean
        # "no ordering available", and the firmware treats an empty version exactly that way.
        print("fwver: git describe failed (%s); FW_VERSION will be empty"
              % (out.stderr or "").strip())
        return ""
    return out.stdout.strip()


ver = _describe()
print("fwver: FW_VERSION=%s" % (ver if ver else "(empty - hash comparison only)"))
# Quoted for the preprocessor with escaped quotes, the form PlatformIO passes through to the
# compiler unmangled on every host. A bare string here becomes an identifier and fails the build.
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(ver))])
