Add-Type -AssemblyName System.Drawing

# scrdock icon: dark rounded plate + screen (left) + docked toolbar (right).
# Authored on a 256-unit canvas, scaled per target size.
function New-Icon([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    function PX($v) { [int]($v * $S / 256) }
    function RoundRect($x, $y, $w, $h, $r) {
        $p = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = 2 * $r
        $p.AddArc($x, $y, $d, $d, 180, 90)
        $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
        $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
        $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
        $p.CloseFigure()
        return $p
    }
    function BR($a, $r_, $g_, $b_) {
        New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($a, $r_, $g_, $b_))
    }
    function PN($r_, $g_, $b_, $w_) {
        New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, $r_, $g_, $b_)), $w_
    }

    # background plate
    $plate = RoundRect (PX 8) (PX 8) (PX 240) (PX 240) (PX 52)
    $g.FillPath((BR 255 31 36 43), $plate)
    $g.DrawPath((PN 74 82 95 3), $plate)

    # screen (left)
    $scr = RoundRect (PX 34) (PX 52) (PX 112) (PX 152) (PX 16)
    $g.FillPath((BR 255 46 86 130), $scr)
    $scrIn = RoundRect (PX 42) (PX 60) (PX 96) (PX 136) (PX 10)
    $g.FillPath((BR 255 60 116 176), $scrIn)
    # display content stripes
    $g.FillRectangle((BR 255 88 148 208), (PX 52), (PX 72), (PX 76), (PX 26))
    $g.FillRectangle((BR 255 88 148 208), (PX 52), (PX 108), (PX 44), (PX 12))

    # dock bar (right) - the toolbar itself
    $dock = RoundRect (PX 166) (PX 44) (PX 56) (PX 168) (PX 20)
    $g.FillPath((BR 255 43 48 55), $dock)
    $g.DrawPath((PN 96 104 118 3), $dock)

    # dock buttons (middle one accent green)
    foreach ($i in 0..2) {
        $y = 60 + $i * 50
        $btn = RoundRect (PX 176) (PX $y) (PX 36) (PX 36) (PX 9)
        if ($i -eq 1) {
            $g.FillPath((BR 255 76 175 80), $btn)
        } else {
            $g.FillPath((BR 255 232 236 240), $btn)
        }
    }

    $g.Dispose()
    return $bmp
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$tmp = "$env:TEMP\scrdock_icon"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$data = @{}
foreach ($s in $sizes) {
    $bmp = New-Icon $s
    $f = "$tmp\$s.png"
    $bmp.Save($f, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $data[$s] = [IO.File]::ReadAllBytes($f)
}

# assemble .ico (PNG entries)
$fs = [System.IO.File]::Create("$PWD\scrdock.ico")
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
foreach ($s in $sizes) {
    $b = $data[$s]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$b.Length)
    $bw.Write([uint32]$offset)
    $offset += $b.Length
}
foreach ($s in $sizes) { $bw.Write($data[$s]) }
$bw.Flush(); $bw.Close()

Copy-Item "$tmp\256.png" "$PWD\_icon_preview.png" -Force
Remove-Item $tmp -Recurse -Force
"ico written: $((Get-Item "$PWD\scrdock.ico").Length) bytes"
