# ---------------------------------------------------------------------------
#  run-software.ps1  -  Launch Trespasser with the original SOFTWARE rasteriser.
#
#  Clears TRESPASS_D3D11 (even if it is set as a persistent user/system var) so
#  the D3D11 backend stays a complete no-op.  Use this as the A/B baseline
#  against run-d3d11.ps1 to compare draw distance / texture sharpness.
#
#  This script lives in the repo; the exe it launches lives in the gitignored
#  build tree, so the location is derived rather than assumed.  The game must be
#  RUN from that folder - it resolves tpass.ini relative to the current
#  directory, and launching from anywhere else makes it default Installed=FALSE
#  and throw "Trespasser is not installed properly".
#
#  Usage, from anywhere:
#     jp2_pc\RunTools\run-software.ps1
#     jp2_pc\RunTools\run-software.ps1 -ExeDir <some other build output>
# ---------------------------------------------------------------------------

param(
    [string] $ExeDir = ''       # build output holding trespass.exe; blank = the default x64 Release
)

$ErrorActionPreference = 'Stop'

if (-not $ExeDir) {
    $ExeDir = Join-Path $PSScriptRoot '..\Build\cmake-x64\cmake\trespass\Release'
}
if (-not (Test-Path -LiteralPath $ExeDir)) {
    throw "build output not found: $ExeDir`nBuild the x64 Release target first, or pass -ExeDir."
}
$ExeDir = (Resolve-Path -LiteralPath $ExeDir).Path

$exe = Join-Path $ExeDir 'trespass.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "trespass.exe not found in the build output: $exe" }

Remove-Item Env:\TRESPASS_D3D11 -ErrorAction SilentlyContinue
Write-Host 'TRESPASS_D3D11 unset  ->  original software rasteriser' -ForegroundColor Cyan
Write-Host "Working dir: $ExeDir"
Write-Host "Launching:   $exe`n"

Push-Location $ExeDir
try { & $exe @args }
finally { Pop-Location }
