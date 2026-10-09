# Picture quality, start-up settings: the same view and windows as the start of
# picture-fov.ps1, for a session started with a setting that the game only takes when
# textures load (r.MaxAnisotropy through [cvars]). Compare with a session without it.
. "$PSScriptRoot\picture-lib.ps1"
Start-Sleep 3
P 'cvar get r.MaxAnisotropy' | Out-Null
P 'fp third' | Out-Null
Start-Sleep 4
Measure-Perf 'start' 30 | Out-Null
Capture-View 'start' -Turning
Measure-Perf 'start2' 30 | Out-Null
P 'fp first' | Out-Null
Start-Sleep 3
Capture-View 'fp'
