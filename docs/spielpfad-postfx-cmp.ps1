param([string[]]$Pairs, [string]$Root = "C:\hw130\s2\shots", [string]$File = "fresh_1600x900.png")
# Thema 130 step 2: A/B of two captures. Below the top 25 % (the sky band
# animates clouds): mean |diff| per channel, pixels that differ at all, and the
# mean luma of each image there.
if (-not ("ImgCmp130" -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class ImgCmp130 {
  static byte[] Px(Bitmap b, out int stride) {
    var d = b.LockBits(new Rectangle(0,0,b.Width,b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    stride = d.Stride; var a = new byte[stride*b.Height]; Marshal.Copy(d.Scan0, a, 0, a.Length); b.UnlockBits(d); return a; }
  public static string Cmp(string pa, string pb) {
    using (var A = new Bitmap(pa)) using (var B = new Bitmap(pb)) {
      if (A.Width != B.Width || A.Height != B.Height) return "size " + A.Width+"x"+A.Height+" vs "+B.Width+"x"+B.Height;
      int sa, sb; var a = Px(A, out sa); var b = Px(B, out sb); int W = A.Width, H = A.Height;
      double tot = 0, la = 0, lb = 0; long n = 0, diff = 0, black = 0;
      for (int y = H/4; y < H; ++y) for (int x = 0; x < W; ++x) {
        int i = y*sa + x*4;
        double d = (Math.Abs(a[i]-b[i]) + Math.Abs(a[i+1]-b[i+1]) + Math.Abs(a[i+2]-b[i+2])) / 3.0;
        tot += d; ++n; if (d > 0) ++diff;
        la += 0.0722*a[i] + 0.7152*a[i+1] + 0.2126*a[i+2];
        lb += 0.0722*b[i] + 0.7152*b[i+1] + 0.2126*b[i+2];
        if (a[i] + a[i+1] + a[i+2] == 0) ++black; }
      return string.Format("{0}x{1} belowSky: meanDiff={2:F2} diffPx={3}/{4} lumaA={5:F1} lumaB={6:F1} blackA={7}", W, H, tot/n, diff, n, la/n, lb/n, black);
    } } }
"@
}
foreach ($p in $Pairs) { $x, $y = $p -split ':'; "{0,-22} {1}" -f $p, [ImgCmp130]::Cmp("$Root\$x\$File", "$Root\$y\$File") }
