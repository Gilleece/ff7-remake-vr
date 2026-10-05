<#
.SYNOPSIS
  Capture the running game's window to a PNG.

.DESCRIPTION
  Default method 'print' uses PrintWindow with PW_RENDERFULLCONTENT, which
  gets the DWM-composed content of the window. It works with the game's D3D11
  flip-model swap chain and while the window is covered by other windows.
  'screen' copies the screen area instead (window brought to the front first).

  Prints the PNG path, its size, mean brightness and the detected screen state
  (title, menu, loading, ...). Exits 2 if the frame is blank (black/flat).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\screenshot.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\screenshot.ps1 -Path captures\title.png
#>
param(
    [string]$Path = '',
    [ValidateSet('print', 'screen')]
    [string]$Method = 'print'
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

if (-not $Path) { $Path = New-CapturePath 'shot' }
elseif (-not [System.IO.Path]::IsPathRooted($Path)) { $Path = Join-Path (Get-Location) $Path }

$r = Save-GameScreenshot -path $Path -method $Method
$bmp = New-Object System.Drawing.Bitmap $r.Path
try { $state = (Get-FrameState $bmp).State } finally { $bmp.Dispose() }
Write-Output ("{0}  {1}x{2}  mean {3}  stddev {4}  state {5}{6}" -f $r.Path, $r.Width, $r.Height, $r.Mean, $r.StdDev, $state,
              $(if ($r.Blank) { '  BLANK' } else { '' }))
if ($r.Blank) { exit 2 }
exit 0
