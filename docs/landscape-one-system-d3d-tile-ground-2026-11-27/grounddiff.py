import sys
from PIL import Image, ImageChops, ImageStat
a, b = sys.argv[1], sys.argv[2]
for i in sys.argv[3:]:
    fa = Image.open(f"{a}/frame_{int(i):05d}.ppm").convert("RGB"); fb = Image.open(f"{b}/frame_{int(i):05d}.ppm").convert("RGB")
    w, h = fa.size; box = (0, int(h * 0.55), w, h)   # the ground below the horizon (camera 700 m, horizon ~ 0.5 h)
    d = ImageChops.difference(fa.crop(box), fb.crop(box))
    print(f"frame {i} ground: mean {sum(ImageStat.Stat(d).mean) / 3:.2f} levels, {sum(1 for p in d.convert('L').get_flattened_data() if p > 30)} px > 30")
