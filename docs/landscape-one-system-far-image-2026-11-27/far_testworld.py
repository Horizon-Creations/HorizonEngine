#!/usr/bin/env python3
"""Writes an asymmetric 40 km test landscape into an old-system scene (TerrainComponent), for the far-image witness (topic 355).
Heights: a 900 m mountain north-east, a 300 m hill south-west, small waves. Layers: 0 grass everywhere, 1 dirt in a big letter F
(not mirror- or transpose-symmetric, so a flipped bake shows), 2 rock on the mountain's steep ring, 3 snow above 550 m.
usage: far_testworld.py in.hescene out.hescene"""
import base64, json, math, struct, sys

src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src))
tc = d["entities"][0]["components"]["terrain"]
SIZE, RES, WRES = 40000.0, 1025, 512
tc.update(sizeX=SIZE, sizeZ=SIZE, resolution=RES, weightRes=WRES, heightScale=1.0)


def height(x, z):   # x, z in metres from the terrain's min corner
    m = 900.0 * math.exp(-((x - 30000) ** 2 + (z - 10000) ** 2) / (6000.0 ** 2))
    hill = 300.0 * math.exp(-((x - 9000) ** 2 + (z - 31000) ** 2) / (5000.0 ** 2))
    return m + hill + 25.0 * math.sin(x / 1700.0) * math.cos(z / 2300.0) + 30.0


hs = [height(i / (RES - 1) * SIZE, j / (RES - 1) * SIZE) for j in range(RES) for i in range(RES)]
tc["sculptHeightsB64"] = base64.b64encode(struct.pack(f"<{len(hs)}f", *hs)).decode()


def in_f(x, z):   # a letter F, 20 km tall, in the west half: stem plus two bars to the +x side, the upper bar longer
    u, v = (x - 3000) / 1000.0, (z - 6000) / 1000.0   # km in the letter's box (v grows with z)
    if not (0 <= u <= 14 and 0 <= v <= 20):
        return False
    return u <= 4 or (v <= 4) or (8 <= v <= 11 and u <= 10)


w = bytearray(WRES * WRES * 4)
for j in range(WRES):
    for i in range(WRES):
        x, z = (i + 0.5) / WRES * SIZE, (j + 0.5) / WRES * SIZE
        h = height(x, z)
        r = math.hypot(x - 30000, z - 10000)
        layer = 3 if h > 550 else 2 if 4000 < r < 7000 and h > 200 else 1 if in_f(x, z) else 0
        w[(j * WRES + i) * 4 + layer] = 255
tc["layerWeightsB64"] = base64.b64encode(bytes(w)).decode()
json.dump(d, open(dst, "w"), indent=2)
print("heights", round(min(hs)), "..", round(max(hs)), "m; layer texels", [sum(1 for k in range(c, len(w), 4) if w[k]) for c in range(4)])
