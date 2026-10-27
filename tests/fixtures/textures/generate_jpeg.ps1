# Writes gradient_9x7.jpg (US-021 fixture): a 9x7 image, an odd size in both directions, at JPEG quality 100.
# Run on Windows:  powershell -File generate_jpeg.ps1
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot "gradient_9x7.jpg"
$bitmap = New-Object System.Drawing.Bitmap 9, 7
for ($y = 0; $y -lt 7; $y++) {
    for ($x = 0; $x -lt 9; $x++) {
        $bitmap.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $x * 28, $y * 36, 128))
    }
}
$encoder = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$parameters = New-Object System.Drawing.Imaging.EncoderParameters 1
$parameters.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, [long]100)
$bitmap.Save($out, $encoder, $parameters)
$bitmap.Dispose()
