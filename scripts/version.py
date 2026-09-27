#!/usr/bin/env python3
"""The build's version, taken from git tags (releases are tag-driven: pushing vX.Y.Z releases X.Y.Z).

$NAM_VERSION wins when set (the release workflow sets it from the tag). Otherwise `git describe`
against the latest v* tag: "1.2.0" on a tagged commit, "1.2.0-3-gabc1234" three commits later,
"-dirty" appended for uncommitted changes, and "0.0.0-dev" with no tag (or no git history).

  scripts/version.py          print the version
  scripts/version.py --vst    print the VST2 version integer, MAJOR*10000 + MINOR*100 + PATCH
"""
import os
import re
import subprocess
import sys
from pathlib import Path

SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                    r"(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?"
                    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$")


def version():
    if os.environ.get("NAM_VERSION"):
        return os.environ["NAM_VERSION"]
    try:
        out = subprocess.run(["git", "describe", "--tags", "--match", "v[0-9]*", "--dirty"],
                             cwd=Path(__file__).resolve().parent, capture_output=True, text=True, check=True)
        return out.stdout.strip()[1:]
    except (OSError, subprocess.CalledProcessError):
        return "0.0.0-dev"


def main():
    v = version()
    m = SEMVER.match(v)
    if not m:
        sys.exit(f"version {v!r} is not a semantic version (vMAJOR.MINOR.PATCH[-PRERELEASE])")
    major, minor, patch = (int(x) for x in m.groups()[:3])
    if minor > 99 or patch > 99:
        sys.exit(f"version {v}: minor and patch must be <= 99 to fit the VST version integer")
    if sys.argv[1:] == ["--vst"]:
        print(major * 10000 + minor * 100 + patch)
    elif not sys.argv[1:]:
        print(v)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
