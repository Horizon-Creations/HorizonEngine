import sys, os, glob
# Sharpness of the near ground: mean |horizontal neighbour difference| of the green channel in the bottom third, averaged over
# every 10th frame from 60 on.
d = sys.argv[1]
vals = []
for p in sorted(glob.glob(os.path.join(d, 'frame_*.ppm')))[60::10]:
    data = open(p, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    acc = n = 0
    for y in range(int(h * 0.67), h, 4):
        row = px[y * w * 3:(y + 1) * w * 3]
        g = row[1::3]
        acc += sum(abs(g[i + 1] - g[i]) for i in range(0, w - 1, 2)); n += (w - 1) // 2 + 1
    vals.append(acc / n)
print('sharpness mean %.3f  first %.3f  last %.3f' % (sum(vals) / len(vals), vals[0], vals[-1]))
