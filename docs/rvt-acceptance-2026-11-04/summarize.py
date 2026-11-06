import json, glob, os, re, statistics, sys
S = os.path.dirname(os.path.abspath(__file__))
prefix = sys.argv[1] if len(sys.argv) > 1 else 'm1080'
rows = {}
for f in sorted(glob.glob(os.path.join(S, f'game_{prefix}_*.json'))):
    m = re.match(rf'game_{prefix}_(\w+?)_rvt(\d)_r(\d+)\.json', os.path.basename(f))
    if not m: continue
    be, rvt, rep = m.group(1), m.group(2), int(m.group(3))
    j = json.load(open(f, encoding='utf-8-sig'))
    su = j['summary']
    gp = su.get('gpuPasses') or {}
    scene = gp.get('Scene', {}).get('avg') if isinstance(gp.get('Scene'), dict) else None
    fr = j['frames']
    draws = fr[len(fr) // 2]['stats'].get('draws') if fr else None
    # backend as the LOG says (the dump's session.backend field is not reliable)
    log = os.path.join(S, f'game_{prefix}_{be}_rvt{rvt}_r{rep}.log')
    drawn = ''
    if os.path.exists(log):
        t = open(log, encoding='utf-8', errors='replace').read()
        mm = re.findall(r'(\d+)/(\d+) segment\(s\) drawn from pages', t)
        drawn = f'{mm[-1][0]}/{mm[-1][1]}' if mm else '-'
        errs = len(re.findall(r'\[ERROR\]', t))
    rows.setdefault((be, rvt), []).append(dict(rep=rep, delta=su['deltaMs']['avg'], gpu=su['gpuFrameMs']['avg'],
                                               cpu=su['cpuFrameMs']['avg'], scene=scene, draws=draws, drawn=drawn,
                                               tris=fr[len(fr)//2]['stats'].get('tris') if fr else None))
print(f'{"backend":8} {"RVT":4} {"n":2} {"frame ms (min / med / max)":30} {"GPU frame ms med":17} {"Scene pass ms med":18} {"CPU ms med":11} draws  from pages')
for (be, rvt), v in sorted(rows.items()):
    d = [x['delta'] for x in v]; g = [x['gpu'] for x in v]; c = [x['cpu'] for x in v]
    sc = [x['scene'] for x in v if x['scene'] is not None]
    print(f'{be:8} {"on" if rvt=="1" else "off":4} {len(v):<2} {min(d):6.3f} / {statistics.median(d):6.3f} / {max(d):6.3f}      '
          f'{statistics.median(g):10.3f}        {statistics.median(sc) if sc else float("nan"):10.3f}         {statistics.median(c):8.3f}   {v[0]["draws"]:<6} {v[0]["drawn"]}')
