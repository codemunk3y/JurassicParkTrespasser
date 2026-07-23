# ---------------------------------------------------------------------------
#  unwedge-mutex.ps1 - close the "DWI Trespasser MUTEX" handle inside a
#  trespass.exe that has begun exiting but deadlocked before releasing it.
#
#  WHY THIS EXISTS: the process's last thread blocks on a user-mode lock during
#  DLL detach (WaitReason 37 = WrAlertByThreadId), so the kernel never reaps the
#  process and never destroys its handles.  The named mutex therefore stays HELD
#  and the next launch exits -1 at main.cpp:423 writing no log at all.
#  TerminateProcess cannot help - the process is already terminating.
#
#  But a handle can be closed from OUTSIDE its owner: DuplicateHandle with
#  DUPLICATE_CLOSE_SOURCE closes the source handle in the target process.  That
#  releases the last reference to the mutex without needing the owner to run
#  another instruction, and without a reboot.
# ---------------------------------------------------------------------------
param([int] $ProcessId = 0)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Unwedge -Name Native -MemberDefinition @'
    [DllImport("ntdll.dll")]
    public static extern int NtQuerySystemInformation(int cls, IntPtr buf, int len, out int ret);
    [DllImport("ntdll.dll")]
    public static extern int NtQueryObject(IntPtr h, int cls, IntPtr buf, int len, out int ret);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool DuplicateHandle(IntPtr srcProc, IntPtr src, IntPtr dstProc,
                                              out IntPtr dst, int access, bool inherit, int options);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")]
    public static extern IntPtr GetCurrentProcess();
'@

$SystemExtendedHandleInformation = 64
$ObjectNameInformation           = 1
$PROCESS_DUP_HANDLE              = 0x0040
$DUPLICATE_SAME_ACCESS           = 0x0002
$DUPLICATE_CLOSE_SOURCE          = 0x0001

# ---- find the target ------------------------------------------------------
if (-not $ProcessId) {
    $rec = Get-CimInstance Win32_Process -Filter "Name='trespass.exe'" | Select-Object -First 1
    if (-not $rec) { Write-Host "No trespass.exe record - nothing to unwedge." -ForegroundColor Green; exit 0 }
    $ProcessId = [int] $rec.ProcessId
}
Write-Host "Target pid $ProcessId" -ForegroundColor Cyan

# The mutex's own type index is not a documented constant, so derive it: create a
# mutex here and find OUR handle in the snapshot.  It must exist BEFORE the
# snapshot is taken, or it simply will not be in it (which is how the first
# version of this script failed).
$mine    = New-Object System.Threading.Mutex($false, "unwedge-probe-$PID")
$mineVal = $mine.SafeWaitHandle.DangerousGetHandle()

# ---- snapshot every handle on the system ----------------------------------
$len = 1MB
$ret = 0
do {
    $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal($len)
    $st  = [Unwedge.Native]::NtQuerySystemInformation($SystemExtendedHandleInformation, $buf, $len, [ref] $ret)
    if ($st -eq 0) { break }
    [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
    $len *= 2
} while ($len -lt 512MB)
if ($st -ne 0) { throw ("NtQuerySystemInformation failed: 0x{0:X8}" -f $st) }

$count  = [Runtime.InteropServices.Marshal]::ReadIntPtr($buf)
$stride = 40                                    # SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, x64
$base   = [IntPtr]::Add($buf, 16)               # past NumberOfHandles + Reserved
Write-Host "$count handles system-wide"

$selfPid  = $PID
$mutantIx = -1

$targets = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt [int]$count; $i++) {
    $e    = [IntPtr]::Add($base, $i * $stride)
    $pid_ = [Runtime.InteropServices.Marshal]::ReadIntPtr($e, 8)
    $hval = [Runtime.InteropServices.Marshal]::ReadIntPtr($e, 16)
    # offset 30, NOT 28: 28 is CreatorBackTraceIndex (nearly always 0), and reading
    # that instead makes every handle look like the type you probed for - which
    # then puts NtQueryObject on file/pipe handles, where it can block forever.
    $tix  = [Runtime.InteropServices.Marshal]::ReadInt16($e, 30)
    if ([int]$pid_ -eq $selfPid -and $hval -eq $mineVal) { $mutantIx = $tix }
    elseif ([int]$pid_ -eq $ProcessId) { [void] $targets.Add(@($hval, $tix)) }
}
if ($mutantIx -lt 0) { throw "could not determine the Mutant type index" }
Write-Host "Mutant type index = $mutantIx; target holds $($targets.Count) handles"

# ---- walk the target's mutants, looking for ours --------------------------
$hProc = [Unwedge.Native]::OpenProcess($PROCESS_DUP_HANDLE, $false, $ProcessId)
if ($hProc -eq [IntPtr]::Zero) { throw ("OpenProcess(PROCESS_DUP_HANDLE) failed, win32 error {0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error()) }

$self    = [Unwedge.Native]::GetCurrentProcess()
$nameBuf = [Runtime.InteropServices.Marshal]::AllocHGlobal(4096)
$closed  = 0

foreach ($t in $targets) {
    if ($t[1] -ne $mutantIx) { continue }        # only mutants - never query unknown types
    $dup = [IntPtr]::Zero
    if (-not [Unwedge.Native]::DuplicateHandle($hProc, $t[0], $self, [ref] $dup, 0, $false, $DUPLICATE_SAME_ACCESS)) { continue }

    $name = ''
    if ([Unwedge.Native]::NtQueryObject($dup, $ObjectNameInformation, $nameBuf, 4096, [ref] $ret) -eq 0) {
        # UNICODE_STRING { USHORT Length; USHORT MaxLength; PWSTR Buffer; }
        $l = [Runtime.InteropServices.Marshal]::ReadInt16($nameBuf, 0)
        $p = [Runtime.InteropServices.Marshal]::ReadIntPtr($nameBuf, 8)
        if ($l -gt 0 -and $p -ne [IntPtr]::Zero) { $name = [Runtime.InteropServices.Marshal]::PtrToStringUni($p, $l / 2) }
    }
    [void] [Unwedge.Native]::CloseHandle($dup)

    if ($name -like '*DWI Trespasser MUTEX*') {
        Write-Host "  found: $name" -ForegroundColor Yellow
        $dup2 = [IntPtr]::Zero
        if ([Unwedge.Native]::DuplicateHandle($hProc, $t[0], $self, [ref] $dup2, 0, $false,
                                              $DUPLICATE_SAME_ACCESS -bor $DUPLICATE_CLOSE_SOURCE)) {
            [void] [Unwedge.Native]::CloseHandle($dup2)
            $closed++
            Write-Host "  closed it in pid $ProcessId" -ForegroundColor Green
        } else {
            Write-Host "  DUPLICATE_CLOSE_SOURCE failed" -ForegroundColor Red
        }
    }
}

[void] [Unwedge.Native]::CloseHandle($hProc)
[Runtime.InteropServices.Marshal]::FreeHGlobal($nameBuf)
[Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
$mine.Dispose()

Write-Host "closed $closed handle(s)"

$r = 'no-mutex (CLEAR - safe to launch)'
try { $mx = [System.Threading.Mutex]::OpenExisting('DWI Trespasser MUTEX')
      if ($mx.WaitOne(1000)) { $mx.ReleaseMutex(); $r = 'exists-but-FREE (safe to launch)' } else { $r = 'STILL HELD' } } catch { }
Write-Host "Mutex now: $r" -ForegroundColor $(if ($r -eq 'STILL HELD') { 'Red' } else { 'Green' })
