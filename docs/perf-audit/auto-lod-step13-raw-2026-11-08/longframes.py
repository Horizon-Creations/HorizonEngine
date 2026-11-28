#!/usr/bin/env python3
"""Share of the long frames spent in WaitForFrame (the nextDrawable wait).
usage: longframes.py <profile.json>...
Pairing trap: frames[i+1].deltaMs is the DURATION of frame i, and frame i's CPU scopes
sit in frames[i]['cpu'] - never pair deltaMs and scopes of the same entry."""
import json, sys
for path in sys.argv[1:]:
    fr = json.load(open(path))['frames']
    n = w = 0
    for i in range(10, len(fr) - 1):
        dt = fr[i + 1]['deltaMs']
        if dt <= 25: continue
        n += 1
        wf = sum(s['ms'] for s in fr[i]['cpu'] if s['n'] == 'WaitForFrame')
        w += wf >= 0.8 * dt
    print(f"{path}: long frames {n}, WaitForFrame >= 80% of the frame: {w}")
