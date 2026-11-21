# PlatformIO pre-build hook: regenerate fonts, icons and the glass background when the source changed.
Import("env")  # noqa: F821
import os
import subprocess
import sys

root = env.subst("$PROJECT_DIR")  # noqa: F821
r = subprocess.run([sys.executable, os.path.join(root, "tools", "gen_assets.py")], cwd=root)
if r.returncode != 0:
    raise SystemExit("[assets] gen_assets.py failed")
