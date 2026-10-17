import struct, zlib, sys
CAP = r"C:\hw150\cap"
def load(name):
    d = open(f"{CAP}\{name}.bmp", "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]; w, h = struct.unpack_from("<ii", d, 18)
    bypp = struct.unpack_from("<H", d, 28)[0] // 8; stride = (w * bypp + 3) & ~3
    flip = h > 0; h = abs(h)
    px = []
    for y in range(h):
        base = off + ((h - 1 - y) if flip else y) * stride
        px.append([(d[base+x*bypp+2], d[base+x*bypp+1], d[base+x*bypp]) for x in range(w)])
    return w, h, px
def png(name, px, w, h, out=None, scale=1):
    rows=[]
    for y in range(0,h,scale):
        r=bytearray([0])
        for x in range(0,w,scale): r+=bytes(px[y][x])
        rows.append(bytes(r))
    def ch(t,dd): return struct.pack(">I",len(dd))+t+dd+struct.pack(">I",zlib.crc32(t+dd)&0xffffffff)
    data=b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",(w+scale-1)//scale,(h+scale-1)//scale,8,2,0,0,0))+ch(b"IDAT",zlib.compress(b"".join(rows)))+ch(b"IEND",b"")
    open(out or f"{CAP}\{name}.png","wb").write(data)
def diff(a,b,band=None,gain=10,out=None):
    w,h,A=load(a); _,_,B=load(b)
    tot=0;n=0;mx=0;big=0;D=[]
    for y in range(h):
        row=[]
        for x in range(w):
            d=max(abs(A[y][x][k]-B[y][x][k]) for k in range(3))
            row.append((min(255,d*gain),)*3)
            if band is None or band[0]<=y<band[1]:
                tot+=sum(abs(A[y][x][k]-B[y][x][k]) for k in range(3))/3; n+=1; mx=max(mx,d); big+= d>8
        D.append(row)
    if out: png(None,D,w,h,out)
    return tot/n, mx, big/n*100
if __name__=="__main__":
    cmd=sys.argv[1]
    if cmd=="png":
        for nm in sys.argv[2:]:
            w,h,p=load(nm); png(nm,p,w,h,scale=2)
    elif cmd=="diff":
        a,b=sys.argv[2],sys.argv[3]
        w,h,_=load(a)
        band=(int(sys.argv[4]),int(sys.argv[5])) if len(sys.argv)>5 else None
        m,mx,big=diff(a,b,band,out=f"{CAP}\diff_{a}_{b}.png")
        print(f"{a} vs {b} band={band}: mean {m:.3f}/255  max {mx}  >8: {big:.3f}%")
