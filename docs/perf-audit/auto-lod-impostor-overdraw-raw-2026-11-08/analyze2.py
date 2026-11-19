import sys, glob, os
S=512; PAD=8; THR=25
def load(p):
    with open(p,'rb') as f:
        for _ in range(4): f.readline()
        return f.read()
def views(data):
    W=2*S
    return [[data[y*W+v*S:y*W+v*S+S] for y in range(S)] for v in range(2)]
def cover(rows,R,gapdiv,maxruns):
    h=S/R; area=0.0; rects=0; prev=[]
    for b in range(R):
        r0=b*S//R; r1=(b+1)*S//R
        col=[False]*S
        for y in range(max(0,r0-PAD),min(S,r1+PAD)):
            r=rows[y]
            for x in range(S):
                if r[x]>=THR: col[x]=True
        g=[False]*S
        for x in range(S):
            if col[x]:
                for k in range(max(0,x-PAD),min(S,x+PAD+1)): g[k]=True
        runs=[]; x=0
        while x<S:
            if g[x]:
                s=x
                while x<S and g[x]: x+=1
                if runs and s-runs[-1][1]<S//gapdiv: runs[-1]=(runs[-1][0],x)
                else: runs.append((s,x))
            else: x+=1
        while len(runs)>maxruns:
            at=min(range(len(runs)-1),key=lambda i:runs[i+1][0]-runs[i][1])
            runs[at]=(runs[at][0],runs[at+1][1]); del runs[at+1]
        now=[]
        for (s,e) in runs:
            m=False
            for i,(ps,pe,py1) in enumerate(prev):
                if ps==s and pe==e and py1==r0: m=True; now.append((s,e,r1)); break
            if not m: now.append((s,e,r1)); rects+=1
        # area: merged rects counted per band anyway
        area+=sum((e-s) for s,e in runs)*(r1-r0)
        prev=now
    return area/(S*S)*100, rects
cfgs=[(8,8,3),(6,8,2),(4,8,2),(4,4,2),(3,4,2),(2,4,2)]
files=sorted(glob.glob(sys.argv[1]+'/*.pgm'))
tot={c:[0,0,0] for c in cfgs}
for p in files:
    for vi,rows in enumerate(views(load(p))):
        for c in cfgs:
            a,r=cover(rows,*c); tot[c][0]+=a; tot[c][1]+=r; tot[c][2]+=1
for c in cfgs:
    print("R=%d gapdiv=%d runs<=%d : mean fill %.1f %%  mean rects/plane %.1f (tris/face-pair %.1f)"%(c+(tot[c][0]/tot[c][2],tot[c][1]/tot[c][2],tot[c][1]/tot[c][2]*2*2)))
