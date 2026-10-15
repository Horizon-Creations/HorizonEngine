#!/usr/bin/env python3
# The caustic net recomputed on the CPU over 40000 random surface points at default knobs (intensity 0.55,
# scale 1, clock 5 s, sun 0.35/0.85/0.2), per floor depth, flat floor facing up, no anti-aliasing (a pixel far
# smaller than the net):
#   old: Thema 185 (postfx_underwatercaustics.frag before Thema 205 step 2): share of the floor the net lights
#        at all (1/|det| > 2), mean net after the depth fade (exp(-depth/14) * smoothstep(0, 0.4, depth)).
#   new: Thema 205 step 2 (the shaders today): share of the floor where the lines add more than 5 % of the
#        sun (light > 0.05), mean added light (x sun), mean share the cells lose (alpha), fade depth 10 m.
# Run: python3 scripts/underwater_caustics_coverage.py

import math, random
T = [(20.0, 1.0, 14.0, 0.07), (335.0, 0.8, 5.0, 0.1), (70.0, 0.5, 1.6, 0.09)]
ETA = 1.33
SUN = (0.35, 0.85, 0.2)


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def trains(chop):
    out = []
    for i in range(6):
        b = i % 3; comp = 1.0 if i >= 3 else 0.0
        d, spd, L, st = T[b]
        out.append((d + comp * 57, L * (1 - 0.27 * comp), st * (1 - 0.4 * comp), spd * (1 + 0.15 * comp)))
    if chop:
        for j in range(1, 11):
            L = 1.2 * (1.0 / 3.0) ** ((j - 1) / 9.0)
            out.append((20.0 + 137.508 * j, L, 0.05, math.sqrt(9.81 / (2 * math.pi) * L)))
    return out


OLD, NEW = trains(False), trains(True)


def hessian(S, tr, scale, t):
    mxx = mzz = mxz = 0.0
    for deg, L, st, spd in tr:
        rad = math.radians(deg); dx, dz = math.cos(rad), math.sin(rad)
        k = 2 * math.pi / L * scale
        c = st * k * math.sin(k * (dx * S[0] + dz * S[1] - spd * t))
        mxx -= c * dx * dx; mzz -= c * dz * dz; mxz -= c * dx * dz
    return mxx, mzz, mxz


def surface_point(Px, Pz, depth, t):
    n = math.sqrt(sum(v * v for v in SUN)); L = [v / n for v in SUN]
    hz = (L[0] / ETA, L[2] / ETA)
    cosT = math.sqrt(max(1 - hz[0] ** 2 - hz[1] ** 2, 0)); ndl = cosT   # floor normal is +Y
    Sx = Px + hz[0] / max(cosT, 0.1) * depth; Sz = Pz + hz[1] / max(cosT, 0.1) * depth
    return (Sx + 1.6 * math.sin(0.43 * Sz + 0.31 * t), Sz + 1.6 * math.sin(0.37 * Sx - 0.27 * t)), ndl


def old_net(Px, Pz, depth, scale=1.0, t=5.0):
    S, _ = surface_point(Px, Pz, depth, t)
    mxx, mzz, mxz = hessian(S, OLD, scale, t)
    kd = (ETA - 1) / ETA * (0.6 * depth + 2.0)
    det = (1 + kd * mxx) * (1 + kd * mzz) - kd * kd * mxz * mxz
    x = max(1 / max(abs(det), 0.1) - 2, 0); n = x / (x + 2)
    return n, n * math.exp(-depth / 14.0) * smoothstep(0.0, 0.4, depth)


def new_net(Px, Pz, depth, intensity=0.55, scale=1.0, t=5.0, fade_depth=10.0):
    if depth <= 0 or depth >= fade_depth:
        return 0.0, 0.0
    S, ndl = surface_point(Px, Pz, depth, t)
    mxx, mzz, mxz = hessian(S, NEW, scale, t)
    kd = (ETA - 1) / ETA * 6.0
    det = (1 + kd * mxx) * (1 + kd * mzz) - kd * kd * mxz * mxz
    td = depth / fade_depth
    fade = (1 - smoothstep(0.0, 1.0, td)) * smoothstep(0.0, 0.15, depth)
    J = 1.0 / max(abs(det), 1.0 / (5.0 + (2.0 - 5.0) * td))
    w = intensity * ndl * fade
    return w * 0.45 * max(J - 1, 0) ** 1.5, 0.6 * min(w, 1.0) * max(1 - J, 0)


random.seed(1)
pts = [(random.uniform(-200, 200), random.uniform(-200, 200)) for _ in range(40000)]
print("depth |  old: cover(net>0)  mean(net*atten) |  new: cover(light>0.05)  mean light  mean darken")
for depth in (0.1, 0.25, 0.5, 1, 2, 3, 5, 8, 9.5, 14):
    o = [old_net(x, z, depth) for x, z in pts]
    n = [new_net(x, z, depth) for x, z in pts]
    oc = sum(1 for a, _ in o if a > 0) / len(o); om = sum(b for _, b in o) / len(o)
    nc = sum(1 for a, _ in n if a > 0.05) / len(n)
    nl = sum(a for a, _ in n) / len(n); nd = sum(b for _, b in n) / len(n)
    print(f"{depth:5.2f} | {oc * 100:15.1f}%  {om:15.4f} | {nc * 100:19.1f}%  {nl:10.3f}  {nd:11.3f}")
