#!/usr/bin/env python3
"""Compares the native build's transcripts of the evaluation corpus with the Electron build's.

    compare-transcripts.py ELECTRON.json MODEL_ID NATIVE.jsonl

ELECTRON.json is the output of scripts/eval-stt.js, and NATIVE.jsonl the output of
`earheart transcribe --json` over the same clips. Prints how many clips both transcribed, how many
texts are identical, and each difference. Exits 1 unless every clip is there and identical.
"""
import json, os, sys

electron, model, native = sys.argv[1:4]
row = next(r for r in json.load(open(electron))["rows"] if r["id"] == model)
want = {d["file"]: d["text"] for d in row["decodes"]}
got = {}
for line in open(native):
    d = json.loads(line)
    got[os.path.basename(d["file"])] = d["text"]
both = sorted(set(want) & set(got))
different = [f for f in both if want[f] != got[f]]
for f in different:
    print(f"{f}\n  electron: {want[f]}\n  native:   {got[f]}")
print(f"{model}: {len(both)} of {len(want)} clips compared, {len(both) - len(different)} identical, {len(different)} different")
sys.exit(0 if len(both) == len(want) and not different else 1)
