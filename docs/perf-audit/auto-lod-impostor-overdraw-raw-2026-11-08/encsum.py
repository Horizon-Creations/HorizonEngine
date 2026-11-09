import sys,re,collections,statistics; sys.path.insert(0,'/private/tmp/auto-lod-step13')
from xtparse import load
sch,rows=load(sys.argv[1])
cb=collections.defaultdict(lambda: collections.defaultdict(lambda: {'Vertex':0,'Fragment':0}))
for r in rows:
    m=re.search(r'Render Command (\d+)',r['event-label'][0]) or re.search(r'(?:Compute|Blit) Command (\d+)',r['event-label'][0])
    k=int(m.group(1)) if m else -1
    cb[r['cmdbuffer-id'][0]][k][r['channel-name'][0]]+=int(r['duration'][1])
cnt=collections.Counter(len(v) for v in cb.values()); typ=cnt.most_common(1)[0][0]
agg=collections.defaultdict(lambda: {'Vertex':[],'Fragment':[]}); nf=0
for c,v in cb.items():
    if len(v)!=typ: continue
    nf+=1
    for k,d in v.items(): agg[k]['Vertex'].append(d['Vertex']/1e6); agg[k]['Fragment'].append(d['Fragment']/1e6)
print('cmdbuffers',len(cb),'typical encoder count',typ,'frames',nf)
tot=0
for k in sorted(agg):
    v=statistics.median(agg[k]['Vertex']); f=statistics.median(agg[k]['Fragment']); tot+=v+f
    if v+f>0.15: print(f"  enc {k:2d}  vertex p50 {v:6.3f}  fragment p50 {f:6.3f}")
print('  sum of all encoders (vertex+fragment, p50 each) %.2f ms'%tot)
