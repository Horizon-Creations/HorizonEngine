import struct,sys
def load(p):
    d=open(p,'rb').read(); off=struct.unpack_from('<I',d,10)[0]; return d[off:]
L={}
def diff(x,y):
    n=0;m=0;s=0
    for i in range(len(x)):
        e=abs(x[i]-y[i])
        if e: n+=1; s+=e; m=max(m,e)
    return n,m,round(s/len(x),4)
names=sys.argv[1:]
for i in range(0,len(names),2):
    a,b=names[i],names[i+1]; print(a,b,'diffbytes,max,mean:',diff(load(a+'.bmp'),load(b+'.bmp')))
