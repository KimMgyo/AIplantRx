"""Build the firmware and publish a GitHub release the devices update from (UI.md 8, INTERFACES.md 5).

    python tools/release.py 2.1.0 --s3 2.1.0 --c3 1.0.1 [--notes "what changed"] [--dry-run] [--prerelease]
    python tools/release.py 2.0.3                      shorthand: only the S3, at the release version

One release is one tested set: every device's image is attached to every release, and manifest.json lists
them all. Devices given a version (--s3, --c3, --cam, --node) are built with it; the others are carried
over from the latest release (same version and image), so they do not reinstall.

1. Writes each new version into that firmware's platformio.ini (-DFW_VERSION) and builds it.
2. Copies the images to dist/v<release>/, downloads the carried-over ones, writes manifest.json:
       {"release", "api", "published", "devices": {"s3": {"version", "file", "url", "size", "sha256"}, ...}}
3. Commits the version changes, tags v<release>, pushes main and the tag, creates the release.

The S3 reads https://github.com/KimMgyo/AIplantRx/releases/latest/download/manifest.json ("latest" = the
newest release that is not a pre-release, so --prerelease publishes without the boards seeing it) and
updates the other devices first, itself last.

Pushing and creating the release need the KimMgyo account (gh auth login --user KimMgyo); the script takes
its token from `gh auth token -u KimMgyo` and never prints it.
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
import urllib.request

REPO = "KimMgyo/AIplantRx"
OWNER_ACCOUNT = "KimMgyo"
API = 1   # INTERFACES.md version
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# device key in the manifest -> (firmware folder, PlatformIO env)
DEVICES = {
    "s3": ("firmware/s3", "s3"),
    "c3": ("firmware/c3", "c3"),
    "cam": ("firmware/cam", "cam"),
    "node": ("firmware/node", "node"),
}
VER_RE = re.compile(r'(-DFW_VERSION=\\")([^\\"]*)(\\")')
VER_OK = re.compile(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?")


def run(cmd, cwd=ROOT, env=None, capture=False):
    print("$", " ".join(cmd))
    r = subprocess.run(cmd, cwd=cwd, env=env, text=True, encoding="utf-8", errors="replace", capture_output=capture)
    if r.returncode != 0:
        if capture:
            print(r.stdout, r.stderr)
        sys.exit(f"failed: {' '.join(cmd)}")
    return r.stdout if capture else ""


def owner_env():
    tok = subprocess.run(["gh", "auth", "token", "-u", OWNER_ACCOUNT], text=True, encoding="utf-8", capture_output=True).stdout.strip()
    if not tok:
        sys.exit(f"no gh token for {OWNER_ACCOUNT}: run gh auth login and sign in as {OWNER_ACCOUNT}")
    env = dict(os.environ)
    env["GH_TOKEN"] = tok
    return env


def set_version(dev, ver):
    ini = os.path.join(ROOT, DEVICES[dev][0], "platformio.ini")
    s = open(ini, encoding="utf-8").read()
    m = VER_RE.search(s)
    if not m:
        sys.exit(f"FW_VERSION not found in {ini}")
    s = VER_RE.sub(lambda m: m.group(1) + ver + m.group(3), s, count=1)
    open(ini, "w", encoding="utf-8", newline="").write(s)
    return m.group(2)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def latest_manifest(env):
    """The manifest of the current latest release, or {} if there is none yet."""
    r = subprocess.run(["gh", "api", f"repos/{REPO}/releases/latest"], env=env, capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        return {}
    rel = json.loads(r.stdout)
    for a in rel.get("assets", []):
        if a["name"] == "manifest.json":
            with urllib.request.urlopen(a["browser_download_url"], timeout=30) as f:
                return json.loads(f.read())
    return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("release", help="release version, e.g. 2.1.0 (no leading v)")
    for d in DEVICES:
        ap.add_argument(f"--{d}", metavar="VER", help=f"build the {d} firmware with this version")
    ap.add_argument("--notes", default="", help="release notes (Markdown)")
    ap.add_argument("--prerelease", action="store_true", help="publish without the boards seeing it")
    ap.add_argument("--dry-run", action="store_true", help="build and write dist/, do not commit or publish")
    a = ap.parse_args()
    rel = a.release.lstrip("v")
    if not VER_OK.fullmatch(rel):
        sys.exit("release must look like 2.1.0 or 2.1.0-rc1")
    tag = "v" + rel
    build = {d: getattr(a, d).lstrip("v") for d in DEVICES if getattr(a, d)}
    if not build:
        build = {"s3": rel}   # the old one-device form
    for d, v in build.items():
        if not VER_OK.fullmatch(v):
            sys.exit(f"--{d} version must look like 1.0.2")
        if not os.path.isdir(os.path.join(ROOT, DEVICES[d][0])):
            sys.exit(f"no firmware folder for {d} ({DEVICES[d][0]})")

    env = owner_env()
    if not a.dry_run:
        if run(["git", "tag", "--list", tag], capture=True).strip() or \
                subprocess.run(["gh", "release", "view", tag, "-R", REPO], env=env, capture_output=True).returncode == 0:
            sys.exit(f"{tag} already exists")
    prev = latest_manifest(env).get("devices", {})

    out = os.path.join(ROOT, "dist", tag)
    os.makedirs(out, exist_ok=True)
    devices = {}
    for d in DEVICES:
        folder, penv = DEVICES[d]
        if d in build:
            old = set_version(d, build[d])
            print(f"{d}: FW_VERSION {old} -> {build[d]}")
            run(["pio", "run", "-e", penv], cwd=os.path.join(ROOT, folder))
            name = f"aiplantrx-{d}-{build[d]}.bin"
            shutil.copyfile(os.path.join(ROOT, folder, ".pio", "build", penv, "firmware.bin"), os.path.join(out, name))
            ver = build[d]
        elif d in prev:
            name, ver = prev[d]["file"], prev[d]["version"]
            print(f"{d}: carried over {ver} from the latest release")
            with urllib.request.urlopen(prev[d]["url"], timeout=120) as f:
                open(os.path.join(out, name), "wb").write(f.read())
            if sha256(os.path.join(out, name)) != prev[d]["sha256"]:
                sys.exit(f"{d}: the carried-over image does not match its sha256")
        else:
            continue   # this device has never been released (no firmware yet)
        path = os.path.join(out, name)
        devices[d] = {"version": ver, "file": name,
                      "url": f"https://github.com/{REPO}/releases/download/{tag}/{name}",
                      "size": os.path.getsize(path), "sha256": sha256(path)}
    manifest = {"release": rel, "api": API,
                "published": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "devices": devices}
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
    files = [os.path.join(out, v["file"]) for v in devices.values()] + [mpath]
    notes = a.notes or "AIplantRx " + tag + "\n\n" + "\n".join(f"- {d}: {v['version']}" for d, v in devices.items())
    cmd = ["gh", "release", "create", tag, *files, "-R", REPO, "--title", f"AIplantRx {tag}", "--notes", notes, "--verify-tag"]
    cmd.append("--prerelease" if a.prerelease else "--latest")
    run(cmd, env=env)
    print(f"published https://github.com/{REPO}/releases/tag/{tag}")


if __name__ == "__main__":
    main()
