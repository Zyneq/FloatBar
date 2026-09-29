# Builds assets\floatbar.ico from the pixel-art master assets\FloatBar_icon.png (32 x 32).
# Whole-number scales (64, 128, 256) stay crisp (nearest neighbour); the rest are
# smoothed. A hand-drawn assets\FloatBar_icon_<size>.png (e.g. _16) replaces the
# generated size.
#
#   powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot '..\assets'
$master = [System.Drawing.Bitmap]::FromFile((Join-Path $assets 'FloatBar_icon.png'))

function Get-Size([int]$size) {
    # The leading commas keep PowerShell from unrolling the byte arrays.
    $drawn = Join-Path $assets "FloatBar_icon_$size.png"
    if (Test-Path $drawn) { return , [System.IO.File]::ReadAllBytes($drawn) }
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.InterpolationMode = if ($size % $master.Width -eq 0) { 'NearestNeighbor' } else { 'HighQualityBicubic' }
    $g.DrawImage($master, 0, 0, $size, $size)
    $g.Dispose()
    $stream = New-Object System.IO.MemoryStream
    $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $stream.ToArray()
}

# ICO: a directory of PNG images (Windows reads PNG entries at every size).
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$images = foreach ($s in $sizes) { , [byte[]](Get-Size $s) }
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i] % 256   # 0 means 256
    $w.Write([byte]$s); $w.Write([byte]$s); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write([byte[]]$img) }
[System.IO.File]::WriteAllBytes((Join-Path $assets 'floatbar.ico'), $out.ToArray())
$master.Dispose()
Write-Output "assets\floatbar.ico: $($sizes -join ', ') px"
