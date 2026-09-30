param([string[]]$Runs, [string]$Root = "C:\hw112\s2\shots")
# Thema 112 step 2: every resized frame against the fresh-start reference of
# the same size. mean = mean |diff| per channel (0..255) over the frame (the
# animated clouds dominate it); belowSkyDiffPx = pixels below the top 25 % that
# differ at all, outsideStart = those outside the 1280x720 start extent (where a stale scene depth would bite after growing); worstBlock = the largest
# 32x32-block mean below the top 25 % (the sky band carries cloud animation).
if (-not ("ImgCmp" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class ImgCmp {
  static byte[] Px(Bitmap b, out int stride) {
    var d = b.LockBits(new Rectangle(0,0,b.Width,b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    stride = d.Stride; var a = new byte[stride*b.Height]; Marshal.Copy(d.Scan0, a, 0, a.Length); b.UnlockBits(d); return a; }
  public static string Cmp(string pa, string pb) {
    using (var A = new Bitmap(pa)) using (var B = new Bitmap(pb)) {
      if (A.Width != B.Width || A.Height != B.Height) return "size " + A.Width+"x"+A.Height+" vs "+B.Width+"x"+B.Height;
      int sa, sb; var a = Px(A, out sa); var b = Px(B, out sb); int W = A.Width, H = A.Height;
      double tot = 0, out_ = 0; long n = 0, no = 0, bsN = 0, bsDiff = 0, bsOutDiff = 0;
      int bw = (W + 31) / 32, bh = (H + 31) / 32; var blk = new double[bw*bh]; var bc = new int[bw*bh];
      for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int i = y*sa + x*4; double d = (Math.Abs(a[i]-b[i]) + Math.Abs(a[i+1]-b[i+1]) + Math.Abs(a[i+2]-b[i+2])) / 3.0;
        tot += d; ++n; if (x >= 1280 || y >= 720) { out_ += d; ++no; } if (y >= H/4) { ++bsN; if (d > 0) { ++bsDiff; if (x >= 1280 || y >= 720) ++bsOutDiff; } }
        int k = (y/32)*bw + (x/32); blk[k] += d; bc[k]++; }
      double worst = 0; int wx = 0, wy = 0;
      for (int by = 0; by < bh; ++by) { if (by*32 < H/4) continue; for (int bx = 0; bx < bw; ++bx) { int k = by*bw+bx; double m = blk[k]/bc[k]; if (m > worst) { worst = m; wx = bx*32; wy = by*32; } } }
      return string.Format("mean={0:F2} worstBlock={1:F1}@({2},{3}) belowSkyDiffPx={4}/{5} (outsideStart={6})", tot/n, worst, wx, wy, bsDiff, bsN, bsOutDiff);
    } } }
"@
}
$refs = @{ "1800x1000" = "ref_1800x1000_a\fresh_1800x1000.png"; "960x540" = "ref_960x540_a\fresh_960x540.png";
           "1600x900" = "ref_1600x900_a\fresh_1600x900.png"; "1280x720" = "ref_1280x720_a\fresh_1280x720.png" }
"noise floor (fresh a vs fresh b, 1800x1000): " + [ImgCmp]::Cmp("$Root\ref_1800x1000_a\fresh_1800x1000.png", "$Root\ref_1800x1000_b\fresh_1800x1000.png")
foreach ($r in $Runs) {
    foreach ($f in Get-ChildItem "$Root\$r" -Filter "*.png" | Sort-Object Name) {
        if ($f.Name -notmatch '(\d+x\d+)\.png$' -and $f.Name -notmatch '^00_start') { continue }
        $size = if ($f.Name -match '^00_start') { "1280x720" } else { $matches[1] }
        if (-not $refs.ContainsKey($size)) { continue }
        "{0,-8} {1,-28} {2}" -f $r, $f.Name, [ImgCmp]::Cmp($f.FullName, "$Root\$($refs[$size])")
    }
}

