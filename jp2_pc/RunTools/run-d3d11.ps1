# ---------------------------------------------------------------------------
#  run-d3d11.ps1  -  Launch Trespasser with the modern D3D11 GPU renderer ON.
#
#  TRESPASS_D3D11 / TRESPASS_DETILE are ENVIRONMENT VARIABLES (not tpass.ini
#  settings); the renderer reads them once at startup.  This script sets them
#  for THIS launch only and - crucially - runs the exe from the BUILD OUTPUT
#  folder so the engine finds tpass.ini there (it resolves the ini relative to
#  the current directory; launching from the wrong folder makes it default
#  Installed=FALSE and throw the "Trespasser is not installed properly" dialog).
#
#  This script lives in the repo; the exe it launches does not (the build tree
#  is gitignored).  By default it drives ..\Build\cmake-x64\cmake\trespass\
#  Release - pass -ExeDir to point it at any other build output.
#
#  Usage, from anywhere:
#     jp2_pc\RunTools\run-d3d11.ps1              # detile strength 1.0 (full)
#     jp2_pc\RunTools\run-d3d11.ps1 -Detile 0.5  # subtler
#     jp2_pc\RunTools\run-d3d11.ps1 -Detile 0    # detile off (plain GPU path)
#     jp2_pc\RunTools\run-d3d11.ps1 -Stereo      # VR stereo, side by side in the window
#     jp2_pc\RunTools\run-d3d11.ps1 -Stereo -Ipd 70  # ...with a 70mm interpupillary distance
#     jp2_pc\RunTools\run-d3d11.ps1 -VR -RuntimeJson C:\Dev\meta\meta_openxr_simulator.json
#
#  -Stereo renders the scene TWICE per frame, once per eye, and splits the
#  window - left eye left, right eye right.  It needs no headset and no OpenXR
#  runtime: it is how the per-eye pipeline is verified on a plain monitor.  Look
#  for near objects shifting between the halves while distant ones stay put -
#  that horizontal shift IS the stereo parallax.  Cross your eyes to fuse them
#  and the scene should have real depth (see VR notes).
#
#  NOTE: leave tpass.ini's  D3D=0  alone - that is the LEGACY 1998 hardware
#  Direct3D driver, unrelated to this, and our path needs it off.
# ---------------------------------------------------------------------------

param(
    [double] $Detile = 1.0,
    [switch] $Stereo,
    [int]    $Ipd = 63,         # interpupillary distance in mm (adult average)
    [switch] $VR,               # turn the OpenXR path on (TRESPASS_VR)
    [string] $RuntimeJson = '', # OpenXR runtime manifest; implies -VR. Blank = system default
    [int]    $VrScale = 100,    # per-eye render size, % of the runtime's recommendation
    [switch] $InputLog,         # (retained for compatibility; input logging is now always on)
    [switch] $Force,            # launch even if another trespass.exe appears to be running
    [string] $ExeDir = ''       # build output holding trespass.exe; blank = the default x64 Release
)

$ErrorActionPreference = 'Stop'

# ---- Where the build output is --------------------------------------------
# This script lives in the REPO (jp2_pc/RunTools) and the exe it launches lives in the build
# tree, which is gitignored - so the two are no longer in the same folder and the location has
# to be derived rather than assumed.  Everything the game touches (tpass.ini, the render log,
# openxr_loader.dll) sits next to the exe, not next to this script, and the game must be RUN
# from there: it resolves tpass.ini relative to the current directory, and launching from
# anywhere else makes it default Installed=FALSE and throw "Trespasser is not installed
# properly".
if (-not $ExeDir) {
    $ExeDir = Join-Path $PSScriptRoot '..\Build\cmake-x64\cmake\trespass\Release'
}
if (-not (Test-Path -LiteralPath $ExeDir)) {
    throw "build output not found: $ExeDir`nBuild the x64 Release target first, or pass -ExeDir."
}
$ExeDir = (Resolve-Path -LiteralPath $ExeDir).Path

$exe = Join-Path $ExeDir 'trespass.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "trespass.exe not found in the build output: $exe" }

$env:TRESPASS_D3D11  = '1'
$env:TRESPASS_DETILE = "$Detile"
Write-Host "TRESPASS_D3D11 = 1   TRESPASS_DETILE = $Detile" -ForegroundColor Cyan

# ---- OpenXR ---------------------------------------------------------------
# Point the OpenXR loader at a specific runtime for THIS PROCESS ONLY, via
# XR_RUNTIME_JSON.  Preferred over the registry ActiveRuntime the simulator's
# own toggle sets: only one runtime can be active system-wide, so the registry
# route means the simulator hijacks OpenXR for every app on the machine and has
# to be toggled back off before a real headset will work.  The env var scopes
# that choice to this launch and needs no admin rights.
Remove-Item Env:\XR_RUNTIME_JSON  -ErrorAction SilentlyContinue
Remove-Item Env:\TRESPASS_VR      -ErrorAction SilentlyContinue
Remove-Item Env:\TRESPASS_VR_SCALE -ErrorAction SilentlyContinue

# A Quest 3 asks for 1680x1760 PER EYE - about 20x the pixels of the 640x480 flat game, twice
# over.  Shrinking the eye targets is the first thing to try if the frame rate collapses; the
# runtime upscales the result, so it costs sharpness and nothing else.
if ($VrScale -ne 100) {
    if ($VrScale -lt 10 -or $VrScale -gt 100) { throw "-VrScale must be between 10 and 100" }
    $env:TRESPASS_VR_SCALE = "$VrScale"
    Write-Host "TRESPASS_VR_SCALE = $VrScale%  (per-eye render size)" -ForegroundColor Green
}

if ($RuntimeJson) {
    if (-not (Test-Path -LiteralPath $RuntimeJson)) { throw "OpenXR runtime manifest not found: $RuntimeJson" }
    $env:XR_RUNTIME_JSON = (Resolve-Path -LiteralPath $RuntimeJson).Path
    Write-Host "XR_RUNTIME_JSON = $($env:XR_RUNTIME_JSON)" -ForegroundColor Green
}
if ($VR -or $RuntimeJson) {
    $env:TRESPASS_VR = '1'
    Write-Host "TRESPASS_VR = 1  (OpenXR session path ON)" -ForegroundColor Green
    if (-not (Test-Path -LiteralPath (Join-Path $ExeDir 'openxr_loader.dll'))) {
        Write-Host "WARNING: openxr_loader.dll is not next to trespass.exe - VR will log 'loader not found' and run flat." -ForegroundColor Yellow
    }
}

# Clear any inherited value so an un-switched run is genuinely mono.
Remove-Item Env:\TRESPASS_VR_STEREO -ErrorAction SilentlyContinue
if ($Stereo) {
    $env:TRESPASS_VR_STEREO = "$Ipd"
    Write-Host "TRESPASS_VR_STEREO = $Ipd  (stereo ON, side-by-side, ${Ipd}mm IPD)" -ForegroundColor Magenta
}
Remove-Item Env:\TRESPASS_INPUTLOG -ErrorAction SilentlyContinue
if ($InputLog) {
    $env:TRESPASS_INPUTLOG = '1'
    Write-Host "TRESPASS_INPUTLOG = 1  (raw mouse mickeys -> DebugLog.txt)" -ForegroundColor Green
}
Write-Host "Working dir: $ExeDir"
Write-Host "Launching:   $exe`n"

# Refuse to start on top of a live instance.  Only ONE process can hold the OpenXR session and
# the exclusive DirectInput mouse, so a second copy sits there doing nothing while looking like
# a launch that failed - and its log wipe destroys the first one's evidence.
# THE GAME ITSELF refuses to run twice: main.cpp creates the named mutex "DWI Trespasser MUTEX"
# and exits with -1 if it cannot take it within a second - before the renderer or VR reach any
# logging point, so the only evidence is an empty log.  -Force skips THIS check but CANNOT get
# past that mutex, so say so plainly rather than implying it is a way through.
#
# LIVENESS IS DECIDED BY Get-Process, NOT CIM.  A process that deadlocks during its own exit
# leaves its Win32_Process record behind until it is reaped, which never happens; that record on
# its own is not a reason to refuse a launch (the mutex probe below is).  Get-Process only lists
# processes that are really there, which is the opposite of what the comment here used to claim.
$live = @(Get-Process -Name 'trespass' -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited })

# When they disagree, SAY SO.  A stale record with no live process is the normal aftermath of the
# xrWaitFrame wedge and is nothing to act on - but silently ignoring it looks identical to the
# check being broken, which cost a round of "it is still printing the warning" once already.
$ghosts = @(Get-CimInstance Win32_Process -Filter "Name='trespass.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessId -notin $live.Id })
if ($ghosts.Count -gt 0) {
    Write-Host "note: $($ghosts.Count) trespass.exe record(s) with no live process (pid $($ghosts.ProcessId -join ', '))." -ForegroundColor DarkGray
}

# THE AUTHORITATIVE SINGLE-INSTANCE TEST IS THE MUTEX, NOT THE PROCESS LIST.
#
# main.cpp:422 creates "DWI Trespasser MUTEX" and returns -1 if it cannot take it within a second,
# before the renderer or VR reach any logging point - so the entire symptom is a launch that exits
# instantly and writes NO trespass_render.log, which reads like a broken build.
#
# A process that deadlocked on its way out can be untracked by Get-Process while still holding
# every kernel handle it owned: it never finished terminating, so nothing was released.
# Process-list liveness said "dead", the mutex said "held", and the mutex was right.  Ask the
# object itself - and see the branch below, which no longer treats that as unrecoverable.
$b_mutex_held = $false
try {
    $mx = [System.Threading.Mutex]::OpenExisting('DWI Trespasser MUTEX')
    if ($mx.WaitOne(1000)) { $mx.ReleaseMutex() } else { $b_mutex_held = $true }
    $mx.Dispose()
} catch [System.Threading.WaitHandleCannotBeOpenedException] {
    # No mutex at all - nothing is running.  The normal case.
} catch {
    Write-Host "note: could not probe the single-instance mutex ($($_.Exception.Message))" -ForegroundColor DarkGray
}

if ($b_mutex_held -and $live.Count -eq 0) {
    # A ZOMBIE HOLDING THE MUTEX IS RECOVERABLE - DO NOT REBOOT.
    #
    # What has happened: the process called ExitProcess, Windows terminated all its threads but
    # one, and that survivor is blocked on a USER-MODE lock (thread wait reason 37,
    # WrAlertByThreadId - the primitive behind SRW locks, critical sections and the loader lock)
    # while running some DLL's DLL_PROCESS_DETACH.  The kernel therefore never reaps the process
    # and never destroys its handles, so "DWI Trespasser MUTEX" stays held and main.cpp:423 makes
    # every new launch exit -1 without writing a log line.
    #
    # taskkill cannot help - the process is already terminating, there is nothing left to
    # terminate - and that used to be read as "only a reboot clears this".  It is not: a handle
    # can be closed from OUTSIDE its owner.  DuplicateHandle with DUPLICATE_CLOSE_SOURCE closes
    # the source handle in the target process, which drops the last reference to the mutex
    # without the deadlocked process having to execute another instruction.
    #
    # The underlying bug (VR was never shut down before process exit, so the OpenXR frame thread
    # died holding a runtime lock) is fixed in main.cpp's Cleanup path; this stays as the recovery
    # for any build that predates it, and for any other DLL that hangs its own detach.
    Write-Host "The single-instance mutex is held by a zombie (pid $($ghosts.ProcessId -join ', ')) that has begun" -ForegroundColor Yellow
    Write-Host "exiting but deadlocked in DLL detach.  Closing its mutex handle from outside..." -ForegroundColor Yellow

    $unwedge = Join-Path $PSScriptRoot 'unwedge-mutex.ps1'
    if (Test-Path -LiteralPath $unwedge) {
        foreach ($g in $ghosts) { & $unwedge -ProcessId ([int]$g.ProcessId) }

        $b_mutex_held = $false
        try {
            $mx = [System.Threading.Mutex]::OpenExisting('DWI Trespasser MUTEX')
            if ($mx.WaitOne(1000)) { $mx.ReleaseMutex() } else { $b_mutex_held = $true }
            $mx.Dispose()
        } catch [System.Threading.WaitHandleCannotBeOpenedException] { }
    } else {
        Write-Host "  (unwedge-mutex.ps1 is not next to this script - cannot clear it automatically)" -ForegroundColor DarkYellow
    }

    if ($b_mutex_held) {
        Write-Host "BLOCKED: the mutex is STILL held after trying to close it." -ForegroundColor Red
        Write-Host "trespass.exe would start, fail to take it, and exit -1 writing no log at all." -ForegroundColor Red
        Write-Host "A reboot will clear it; -Force only skips this check, it cannot get past the mutex." -ForegroundColor Red
        if (-not $Force) { return }
        Write-Host "-Force given: launching anyway (expect an immediate silent exit)." -ForegroundColor DarkYellow
    } else {
        Write-Host "Mutex released - launching normally." -ForegroundColor Green
    }
}

if ($live.Count -gt 0) {
    Write-Host "trespass.exe is genuinely running (pid $($live.Id -join ', '), started $($live[0].StartTime.ToString('HH:mm:ss')))." -ForegroundColor Yellow
    Write-Host "The GAME holds a single-instance mutex, so a second copy exits immediately and" -ForegroundColor Yellow
    Write-Host "writes no log at all - -Force cannot bypass that, only this warning." -ForegroundColor Yellow
    Write-Host "Close it, or: Stop-Process -Id $($live.Id -join ',') -Force" -ForegroundColor Yellow
    if (-not $Force) { return }
    Write-Host "-Force given: launching anyway (expect an immediate silent exit)." -ForegroundColor DarkYellow
}

# ROTATE the renderer/VR log rather than deleting it: the previous run is often the interesting
# one, and wiping it on the next launch (or on a retry after a failed start) throws away the
# evidence just as it becomes wanted.  Both RenderD3D11 and RenderVR append here; they also emit
# via OutputDebugString, but that is invisible without a debugger, so this file is what makes a
# finished run diagnosable at all.
$rlog = Join-Path $ExeDir 'trespass_render.log'
$rprev = Join-Path $ExeDir 'trespass_render.prev.log'
if (Test-Path -LiteralPath $rlog) {
    Remove-Item -LiteralPath $rprev -ErrorAction SilentlyContinue
    Move-Item -LiteralPath $rlog -Destination $rprev -ErrorAction SilentlyContinue
    Write-Host "previous log kept as trespass_render.prev.log" -ForegroundColor DarkGray
}

Push-Location $ExeDir
try { & $exe @args }
finally { Pop-Location }

# ---- What actually happened this run --------------------------------------
if (Test-Path -LiteralPath $rlog) {
    Write-Host "`n--- trespass_render.log ---" -ForegroundColor Cyan
    Get-Content -LiteralPath $rlog | ForEach-Object {
        $c = if ($_ -match 'FAIL|failed|not found|no head-mounted|lacks|unsupported') { 'Red' }
             elseif ($_ -match 'OK|created|begun|engaged') { 'Green' }
             else { 'Gray' }
        Write-Host "  $_" -ForegroundColor $c
    }
    if (-not (Select-String -LiteralPath $rlog -SimpleMatch 'device + swap chain created' -ErrorAction SilentlyContinue)) {
        Write-Host "`nWARNING: D3D11 never created its device - it fell back to software," -ForegroundColor Yellow
        Write-Host "         and VR cannot start without it (the session needs that device)." -ForegroundColor Yellow
    }
} else {
    Write-Host "`nWARNING: no trespass_render.log was written at all." -ForegroundColor Yellow
    Write-Host "         Neither the D3D11 backend nor VR reached any logging point -" -ForegroundColor Yellow
    Write-Host "         check TRESPASS_D3D11 is set and that you got in-game (the device" -ForegroundColor Yellow
    Write-Host "         is created on the first painted frame, not at the menu)." -ForegroundColor Yellow
}
