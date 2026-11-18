# Downscale a PNG to W x H (high-quality bicubic) and print its RGB bytes as one hex line (for shot_check.py:
# no Python image library needed; Windows PowerShell's System.Drawing decodes the PNG).
param([Parameter(Mandatory = $true)][string]$Path, [int]$W = 128, [int]$H = 72)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile($Path)
$bmp = New-Object System.Drawing.Bitmap($W, $H, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.DrawImage($src, 0, 0, $W, $H)
$g.Dispose(); $src.Dispose()
$sb = New-Object System.Text.StringBuilder ($W * $H * 6)
for ($y = 0; $y -lt $H; $y++) {
  for ($x = 0; $x -lt $W; $x++) {
    $c = $bmp.GetPixel($x, $y)
    [void]$sb.AppendFormat('{0:x2}{1:x2}{2:x2}', $c.R, $c.G, $c.B)
  }
}
$bmp.Dispose()
$sb.ToString()
