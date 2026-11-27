import sys, os, glob
# Mean absolute difference of consecutive PPM frames (lower 60 % of the picture: the ground), printed for every frame whose
# difference stands out against the median of its +-10 neighbours.
d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, 'frame_*.ppm')))
def load(p):
    with open(p, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    return w, h, px
diffs = []
prev = None
for p in files:
    w, h, px = load(p)
    y0 = int(h * 0.4)
    cur = px[y0 * w * 3:]
    if prev is not None:
        step = 97  # sample every 97th byte
        s = sum(abs(a - b) for a, b in zip(cur[::step], prev[::step]))
        diffs.append(s / (len(cur) // step))
    else:
        diffs.append(0.0)
    prev = cur
import statistics
for i in range(1, len(diffs)):
    nb = diffs[max(1, i - 10):i] + diffs[i + 1:i + 11]
    med = statistics.median(nb) if nb else 0
    if diffs[i] > 1.8 * med + 0.5:
        print(i, round(diffs[i], 2), 'median', round(med, 2))
print('overall median', round(statistics.median(diffs[1:]), 2))
