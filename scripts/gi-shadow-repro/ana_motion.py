# Occluder-motion ghost metric for GI shadow captures (Thema 131, step 2).
# usage: ana_motion.py NEW.bmp OLD.bmp MOVED.bmp [label]
#   NEW   = static capture at the real TOD (settled, e.g. HE_DUMP_FRAMES=60)
#   OLD   = static capture at TOD - step (where the shadow was before the move)
#   MOVED = HE_DUMP_TODSTEP=step capture: settled at TOD - step, then TWO frames
#           at TOD (one with HE_DUMP_TODSTEPFRAMES=1, Thema 146)
# swept = floor pixels where OLD and NEW differ by > 8 grey levels (the area the
# shadow edge moved over). ghost = mean |MOVED - NEW| / mean |OLD - NEW| there:
# 0 = the moved edge is fully in place after one frame, 1 = the old shadow is
# still there untouched. The clamp exists to keep this small.
import sys, numpy as np
sys.argv, args = sys.argv[:1], sys.argv[1:]
exec(open(__file__.replace('ana_motion.py', 'ana.py')).read().split('ref, a0, a1')[0])
new, old, mov = lum(args[0]), lum(args[1]), lum(args[2])
swept = np.abs(old - new) > 8.0
swept[:80] = False                                   # floor region only
d_ref = np.abs(old - new)[swept].mean()
d_mov = np.abs(mov - new)[swept].mean()
lab = args[3] if len(args) > 3 else args[2]
print(f"{lab:28s} swept_px={swept.sum():6d} |old-new|={d_ref:6.2f} |moved-new|={d_mov:6.2f} "
      f"ghost={d_mov / d_ref:5.3f} p99|moved-new|={np.percentile(np.abs(mov - new)[swept], 99):5.1f}")
