#!/usr/bin/env python3
"""Check that vst/params.json, the P_* enum and the PINFO table in vst/nam_vst.cpp list the
parameters in the same order.

MPC stores Q-Link and automation mappings by parameter index, so the three must stay aligned
(README "How it works"). Names are compared with underscores dropped and case folded; ALIASES
covers the few params whose JSON key and C++ name were spelled differently from the start.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALIASES = {"cabir": "cabselect", "cabirprev": "cabprev", "cabirnext": "cabnext"}


def norm(s):
    return s.replace("_", "").lower()


def main():
    keys = [p["key"] for p in json.loads((ROOT / "vst/params.json").read_text())["params"]]
    src = (ROOT / "vst/nam_vst.cpp").read_text()

    m = re.search(r"enum\s*\{\s*(P_MODEL\s*=\s*0\b.*?)\bNPARAMS\b", src, re.S)
    if not m:
        sys.exit("check_params: couldn't find the P_MODEL = 0 ... NPARAMS enum in nam_vst.cpp")
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", m.group(1), flags=re.S)
    enum = [e.split("=")[0].strip()[2:] for e in body.split(",") if e.strip()]
    pinfo = re.findall(r"/\*P_(\w+)\*/\s*\{", src)

    want = [ALIASES.get(norm(k), norm(k)) for k in keys]
    bad = 0
    for label, got in (("P_* enum", enum), ("PINFO", pinfo)):
        if len(got) != len(keys):
            print(f"{label}: {len(got)} entries, params.json has {len(keys)}")
            bad += 1
        for i, (k, w, g) in enumerate(zip(keys, want, got)):
            if w != norm(g):
                print(f"{label}[{i}]: P_{g}, params.json has {k!r}")
                bad += 1
    if bad:
        sys.exit(f"check_params: {bad} mismatch(es)")
    print(f"check_params: {len(keys)} params aligned (params.json, P_* enum, PINFO)")


if __name__ == "__main__":
    main()
