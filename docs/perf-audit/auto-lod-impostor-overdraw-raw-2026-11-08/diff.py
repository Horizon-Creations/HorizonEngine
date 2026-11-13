import sys
def rd(p):
    f=open(p,'rb'); assert f.readline().strip()==b'P6'; w,h=map(int,f.readline().split()); f.readline(); return w,h,f.read()
w,h,a=rd(sys.argv[1]); _,_,b=rd(sys.argv[2])
def plant(d,i):
    r,g,bl=d[i],d[i+1],d[i+2]
    return (r-g>60 and bl-g>60) or False
n=0; ch=0; big=0; pa=0; pb=0; both=0; sumabs=0
for i in range(0,len(a),3):
    da=max(abs(a[i]-b[i]),abs(a[i+1]-b[i+1]),abs(a[i+2]-b[i+2]))
    sumabs+=da
    if da>16: ch+=1
    if da>48: big+=1
npx=w*h
print('pixels',npx,'mean abs diff %.3f'%(sumabs/npx),'>16: %.3f%%'%(ch/npx*100),'>48: %.3f%%'%(big/npx*100))
