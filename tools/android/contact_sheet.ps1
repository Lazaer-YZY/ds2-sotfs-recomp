# Tile screenshots into one contact sheet (cols x rows of W x H thumbnails, file name drawn on each).
#   powershell -File contact_sheet.ps1 -Out sheet.png -List "a.png;b.png;..." [-Cols 5 -W 384 -H 216]
param([Parameter(Mandatory = $true)][string]$Out, [Parameter(Mandatory = $true)][string]$List, [int]$Cols = 5,
      [int]$W = 384, [int]$H = 216)
$Files = $List.Split(';') | Where-Object { $_ }
Add-Type -AssemblyName System.Drawing
$rows = [Math]::Ceiling($Files.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap(($Cols * $W), ($rows * $H))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = New-Object System.Drawing.Font('Arial', 14, [System.Drawing.FontStyle]::Bold)
for ($i = 0; $i -lt $Files.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile((Resolve-Path $Files[$i]).Path)
  $x = ($i % $Cols) * $W; $y = [Math]::Floor($i / $Cols) * $H
  $g.DrawImage($img, $x, $y, $W, $H)
  $g.DrawString([System.IO.Path]::GetFileNameWithoutExtension($Files[$i]), $font, [System.Drawing.Brushes]::Yellow, $x + 4, $y + 4)
  $img.Dispose()
}
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $sheet.Dispose()
