"""Build the firmware and publish a GitHub release the devices update from (UI.md 8).

    python tools/release.py 2.0.2 [--notes "what changed"] [--dry-run]

1. Writes the version into firmware/s3/platformio.ini (-DFW_VERSION) and builds `pio run -e s3`.
2. Copies the image to dist/v<version>/aiplantrx-s3-<version>.bin and writes manifest.json next to it:
       {"release": "2.0.2", "published": "...",
        "devices": {"s3": {"version", "file", "url", "size", "sha256"}}}
   The url points at this release's asset, so a manifest read through releases/latest always names
   files of the same release.
3. Commits the version change, tags v<version>, pushes main and the tag, and creates the release with
   the image and manifest.json as assets.

The S3 reads https://github.com/KimMgyo/AIplantRx/releases/latest/download/manifest.json. "latest" is
the newest release that is not a pre-release, so --prerelease publishes without the boards seeing it.

Pushing and creating the release need the KimMgyo account (gh auth login --user KimMgyo); the script
takes its token from `gh auth token -u KimMgyo` and never prints it.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

REPO = "KimMgyo/AIplantRx"
OWNER_ACCOUNT = "KimMgyo"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S3_DIR = os.path.join(ROOT, "firmware", "s3")
S3_INI = os.path.join(S3_DIR, "platformio.ini")
VER_RE = re.compile(r'(-DFW_VERSION=\\")([^\\"]*)(\\")')


def run(cmd, cwd=ROOT, env=None, capture=False):
    print("$", " ".join(cmd))
    r = subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=capture)
    if r.returncode != 0:
        if capture:
            print(r.stdout, r.stderr)
        sys.exit(f"failed: {' '.join(cmd)}")
    return r.stdout if capture else ""


def owner_env():
    tok = subprocess.run(["gh", "auth", "token", "-u", OWNER_ACCOUNT], text=True, capture_output=True).stdout.strip()
    if not tok:
        sys.exit(f"no gh token for {OWNER_ACCOUNT}: run gh auth login and sign in as {OWNER_ACCOUNT}")
    env = dict(os.environ)
    env["GH_TOKEN"] = tok
    return env


def set_version(ver):
    s = open(S3_INI, encoding="utf-8").read()
    if not VER_RE.search(s):
        sys.exit("FW_VERSION not found in platformio.ini")
    old = VER_RE.search(s).group(2)
    s = VER_RE.sub(lambda m: m.group(1) + ver + m.group(3), s, count=1)
    open(S3_INI, "w", encoding="utf-8", newline="").write(s)
    return old


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version", help="for example 2.0.2 (no leading v)")
    ap.add_argument("--notes", default="", help="release notes (Markdown)")
    ap.add_argument("--prerelease", action="store_true", help="publish without the boards seeing it")
    ap.add_argument("--dry-run", action="store_true", help="build and write dist/, do not commit or publish")
    a = ap.parse_args()
    ver = a.version.lstrip("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", ver):
        sys.exit("version must look like 2.0.2 or 2.1.0-rc1")
    tag = "v" + ver

    env = owner_env()
    if not a.dry_run:
        tags = run(["git", "tag", "--list", tag], capture=True).strip()
        remote = subprocess.run(["gh", "release", "view", tag, "-R", REPO], env=env, capture_output=True, text=True)
        if tags or remote.returncode == 0:
            sys.exit(f"{tag} already exists")
        dirty = run(["git", "status", "--porcelain"], capture=True).strip()
        if dirty:
            print("uncommitted changes will be part of this release commit:")
            print(dirty)

    old = set_version(ver)
    print(f"FW_VERSION {old} -> {ver}")
    run(["pio", "run", "-e", "s3"], cwd=S3_DIR)

    out = os.path.join(ROOT, "dist", tag)
    os.makedirs(out, exist_ok=True)
    name = f"aiplantrx-s3-{ver}.bin"
    img = os.path.join(out, name)
    shutil.copyfile(os.path.join(S3_DIR, ".pio", "build", "s3", "firmware.bin"), img)
    manifest = {
        "release": ver,
        "published": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "devices": {
            "s3": {
                "version": ver,
                "file": name,
                "url": f"https://github.com/{REPO}/releases/download/{tag}/{name}",
                "size": os.path.getsize(img),
                "sha256": sha256(img),
            }
        },
    }
    mpath = os.path.join(out, "manifest.json")
    with open(mpath, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(json.dumps(manifest, indent=2))
    if a.dry_run:
        print(f"dry run: {out}")
        return

    run(["git", "add", "-A"])
    if run(["git", "status", "--porcelain"], capture=True).strip():
        run(["git", "commit", "-q", "-m", f"Release {tag}"])
    run(["git", "tag", "-a", tag, "-m", f"AIplantRx {tag}"])
    helper = ["-c", "credential.helper=", "-c", "credential.helper=!gh auth git-credential"]
    run(["git", *helper, "push", "origin", "main", tag], env=env)
    notes = a.notes or f"AIplantRx {tag}"
    cmd = ["gh", "release", "create", tag, img, mpath, "-R", REPO, "--title", f"AIplantRx {tag}", "--notes", notes,
           "--verify-tag"]
    if a.prerelease:
        cmd.append("--prerelease")
    else:
        cmd.append("--latest")
    run(cmd, env=env)
    print(f"published https://github.com/{REPO}/releases/tag/{tag}")


if __name__ == "__main__":
    main()
