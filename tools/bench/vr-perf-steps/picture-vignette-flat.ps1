# The lens vignette setting only acts while stereo renders: with 3D off (the game on the
# virtual screen) the captures with `stereo vignette 0` and `1` must be the same, and the
# flat picture keeps the game's vignette.
. "$PSScriptRoot\picture-lib.ps1"
Start-Sleep 3
P 'fp third' | Out-Null
Start-Sleep 4
P 'stereo vignette 0' | Out-Null
Start-Sleep 2
Capture-View 'stereo-vig0'
P 'stereo off' | Out-Null
Start-Sleep 5
Capture-View 'flat-vig0'
P 'stereo vignette 1' | Out-Null
Start-Sleep 2
Capture-View 'flat-vig1'
P 'stereo vignette 0' | Out-Null
Start-Sleep 2
Capture-View 'flat-vig0b'
P 'stereo vignette;stereo on' | Out-Null
Start-Sleep 5
Capture-View 'stereo-vig0b'
P 'stereo vignette;fp first' | Out-Null
