#!/usr/bin/env python3
# Thema 205 Schritt 1: the caustic net of postfx_underwatercaustics.frag / OpenGLRenderer.cpp kUnderwaterCausticsFS,
# recomputed on the CPU at default knobs (scale 1, fade depth 14 m, clock 5 s) over 40000 random surface points:
# share of the floor the net lights at all (1/|det| > 2), mean net, mean net after the depth fade, per floor depth.

import math, random
T=[(20.0,1.0,14.0,0.07),(335.0,0.8,5.0,0.1),(70.0,0.5,1.6,0.09)]
eta=1.33
def net(Sx,Sz,depth,scale=1.0,t=5.0):
    Sx+=1.6*math.sin(0.43*Sz+0.31*t); Sz+=1.6*math.sin(0.37*Sx-0.27*t)
    mxx=mzz=mxz=0.0
    for i in range(6):
        b=i%3; comp=1.0 if i>=3 else 0.0
        d,spd,L,st=T[b]; rad=math.radians(d+comp*57)
        dx,dz=math.cos(rad),math.sin(rad)
        k=2*math.pi/(L*(1-0.27*comp))*scale; stp=st*(1-0.4*comp); sp=spd*(1+0.15*comp)
        c=stp*k*math.sin(k*(dx*Sx+dz*Sz-sp*t))
        mxx-=c*dx*dx; mzz-=c*dz*dz; mxz-=c*dx*dz
    kd=(eta-1)/eta*(0.6*depth+2.0)
    det=(1+kd*mxx)*(1+kd*mzz)-kd*kd*mxz*mxz
    x=max(1/max(abs(det),0.1)-2,0); n=x/(x+2)
    atten=math.exp(-depth/14.0)*(lambda e: e*e*(3-2*e))(min(max(depth/0.4,0),1))
    return n,n*atten
random.seed(1)
pts=[(random.uniform(-200,200),random.uniform(-200,200)) for _ in range(40000)]
print("depth  cover(net>0)  mean(net)  mean(net*atten)  max(net*atten)")
for depth in (0.1,0.25,0.5,1,2,3,5,8,14,25):
    v=[net(x,z,depth) for x,z in pts]
    cov=sum(1 for a,_ in v if a>0)/len(v)
    print(f"{depth:5.2f}  {cov*100:8.2f}%  {sum(a for a,_ in v)/len(v):9.4f}  {sum(b for _,b in v)/len(v):12.4f}  {max(b for _,b in v):10.3f}")
