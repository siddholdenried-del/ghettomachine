#!/bin/sh
# Reject a newer/different app paired with an old compiled browser or device build.
# Invoke from the repository root; it performs only read-only local checks.
set -eu
python3 -B - <<'PY'
import json
from pathlib import Path
import subprocess
import sys

def git(*args):
    return subprocess.check_output(("git",*args),text=True).strip()
s=json.loads(Path("release/config.json").read_text())["source"]
assert s.get("runtime_tree"),"release profile must pin the app tree"
if git("rev-parse","HEAD:app")!=s["runtime_tree"] or git("rev-parse",s["emulator_build_commit"]+":app")!=s["runtime_tree"]:
    sys.exit("release: archived emulator and current app source trees differ")
if subprocess.run(("git","merge-base","--is-ancestor",s["emulator_build_commit"],"HEAD"),check=False).returncode:
    sys.exit("release: browser build commit is not in release ancestry")
if subprocess.run(("git","merge-base","--is-ancestor",s["remote_main_predecessor"],"HEAD"),check=False).returncode:
    sys.exit("release: reviewed remote main is not in release ancestry")
if git("ls-tree","HEAD","third_party/fm1-base").split()[2]!=s["fm1_base"]:
    sys.exit("release: fm1-base gitlink differs from the browser build pin")
if git("status","--porcelain","--","app",".gitmodules"):
    sys.exit("release: dirty tracked or untracked app inputs")
print("Verified browser runtime source tree, current-main ancestry and fm1-base gitlink")
PY
