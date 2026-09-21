# pack.ps1
# Merges each data\<category>\<n>\*.jpg folder into a single
# data\<category>\<n>.mjp file, then deletes the folder.
#
# File format:
#   "MJP1"                  4 bytes
#   frame count             uint32, little endian
#   offsets[count + 1]      uint32 each, absolute byte offsets
#   jpeg data               concatenated, in order
#
# Run from random_esp32_testing:   .\pack.ps1

if (-not (Test-Path data)) {
  Write-Host "No data folder here. Run this from random_esp32_testing."
  exit
}

Get-ChildItem data -Directory | ForEach-Object {
  $cat = $_
  Get-ChildItem $cat.FullName -Directory | ForEach-Object {
    $variant = $_
    $files = Get-ChildItem $variant.FullName -Filter *.jpg | Sort-Object Name
    if ($files.Count -eq 0) { return }

    $out = Join-Path $cat.FullName ("{0}.mjp" -f $variant.Name)
    $fs = [System.IO.File]::Create($out)
    $bw = New-Object System.IO.BinaryWriter($fs)

    $count = $files.Count
    $headerSize = 8 + 4 * ($count + 1)

    $bw.Write([byte[]][char[]]'MJP1')
    $bw.Write([uint32]$count)

    $offsets = New-Object uint32[] ($count + 1)
    $pos = [uint32]$headerSize
    for ($i = 0; $i -lt $count; $i++) {
      $offsets[$i] = $pos
      $pos = $pos + [uint32]$files[$i].Length
    }
    $offsets[$count] = $pos
    foreach ($o in $offsets) { $bw.Write([uint32]$o) }

    foreach ($f in $files) { $bw.Write([System.IO.File]::ReadAllBytes($f.FullName)) }

    $bw.Close()
    $fs.Close()
    Remove-Item $variant.FullName -Recurse -Force

    "{0}\{1}.mjp  {2} frames  {3:N0} KB" -f $cat.Name, $variant.Name, $count, ((Get-Item $out).Length / 1KB)
  }
}

""
"{0:N2} MB total  (keep under 1.8)" -f ((Get-ChildItem data -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
""
"Now run:  platformio.exe run --target uploadfs --environment upesy_wroom"
