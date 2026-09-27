#!/usr/bin/env python3
"""vst.json -> build/pluginlist-entry.xml: this effect's <PLUGIN> line for MPC.settings' pluginList-arm.

mpc-vst-plugins' gen_vst.py writes an instrument entry (0 inputs, category Synth, /sdcard/vst/); NAM
is an effect (2 in / 2 out) installed at vst.json's install_path, next to its models folder.
Run after gen_vst.py, which overwrites the file.
"""
import json
import os

ROOT = os.path.dirname(os.path.abspath(__file__))
c = json.load(open(os.path.join(ROOT, "vst.json")))
os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
open(os.path.join(ROOT, "build", "pluginlist-entry.xml"), "w").write(
    '<PLUGIN name="{n}" descriptiveName="{n}" format="VST" category="Effect" manufacturer="{v}" version="1.0" '
    'file="{f}" uid="{u:x}" isInstrument="0" fileTime="0" infoUpdateTime="0" numInputs="2" '
    'numOutputs="2" isShell="0"/>\n'.format(n=c["name"], v=c["vendor"], f=c["install_path"],
                                           u=int.from_bytes(c["uid"].encode(), "big")))
