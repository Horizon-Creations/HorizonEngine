# Edge-band metrics against a converged reference (Thema 134).
# usage: ana134.py GT.bmp A_f60.bmp A_f61.bmp [label]
#   GT = the same view rendered with many rays, high history weight and NO
#        spatial filter (HE_GI_PROTO_SPP=256 HE_GI_PROTO_HIST=0.98
#        HE_GI_PROTO_ATROUS=-1), i.e. the penumbra the mask should converge to.
# ana.py only measures how much a capture CHANGES (flicker) and how grainy it
# is (hf); a wider blur wins both by smearing the edge. Here the error against
# the reference is measured too, so over-blurring shows up as error:
#   flicker = mean |f61 - f60| (p99)      noise over time
#   rmse    = sqrt(mean (f60 - GT)^2)     noise + bias in one frame
#   bias    = |box(f60 - GT, 3)| mean     the smooth (low-frequency) part of the
#             error, i.e. a moved/widened/narrowed edge, not grain
#   sharp   = rmse on the hardest-edge tenth of the band (contact shadows):
#             where an edge-unaware filter would widen the penumbra.
# Band = floor pixels where GT has a shadow gradient (same rule as ana.py).
import sys, numpy as np
sys.argv, args = sys.argv[:1], sys.argv[1:]
exec(open(__file__.replace('ana134.py', 'ana.py')).read().split('ref, a0, a1')[0])
gt, a0, a1 = lum(args[0]), lum(args[1]), lum(args[2])
sm = box(gt, 2)
gy, gx = np.gradient(sm)
g = np.hypot(gx, gy)
band = (g > 1.5); band[:80] = False
sharp = band & (g >= np.percentile(g[band], 90))
flick = np.abs(a1 - a0)
err = a0 - gt
lowf = box(err, 3)
lab = args[3] if len(args) > 3 else args[1]
print(f"{lab:34s} band={band.sum():6d} flicker={flick[band].mean():6.3f} (p99 {np.percentile(flick[band], 99):5.1f}) "
      f"rmse={np.sqrt((err[band] ** 2).mean()):6.3f} bias={np.abs(lowf[band]).mean():6.3f} "
      f"sharp_rmse={np.sqrt((err[sharp] ** 2).mean()):6.3f} hf={np.abs(a0 - box(a0, 2))[band].mean():6.3f}")
