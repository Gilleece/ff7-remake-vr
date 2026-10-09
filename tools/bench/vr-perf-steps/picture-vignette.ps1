# The game's lens vignette in stereo ([stereo] game_vignette, dev command `stereo vignette
# <share>`), third person in the view where the latest save starts: the start of
# picture-fov.ps1 (so a session started with a start-up setting such as r.MaxAnisotropy under
# [cvars] compares with it), then captures with the vignette removed, at half and as in the
# flat game, and A/B frame times. tools\re\vignette_check.py measures the captures.
. "$PSScriptRoot\picture-lib.ps1"
Start-Sleep 3
P 'cvar get r.MaxAnisotropy;stereo vignette' | Out-Null
P 'fp third' | Out-Null
Start-Sleep 4
Measure-Perf 'start' 30 | Out-Null
Capture-View 'start' -Turning
Measure-Perf 'start2' 30 | Out-Null
foreach ($v in @('0', '1', '0.5', '0', '1')) {
    P "stereo vignette $v" | Out-Null
    Start-Sleep 2
    Capture-View "vig$v-$(Get-Date -Format HHmmss)"
}
AB-Commands 'vignette' 'stereo vignette 1' 'stereo vignette 0'
P 'stereo vignette' | Out-Null
P 'fp first' | Out-Null
Start-Sleep 3
P 'stereo vignette 0' | Out-Null
Start-Sleep 2
Capture-View 'fp-vig0'
P 'stereo vignette 1' | Out-Null
Start-Sleep 2
Capture-View 'fp-vig1'
P 'stereo vignette 0;stereo vignette' | Out-Null
