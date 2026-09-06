<#
.SYNOPSIS
  Filesystem conformance check for a wsldrive mount on WINDOWS (WinFsp).

  The Windows counterpart of fs-conformance.sh: mounts a real drive letter and
  exercises the operations tools depend on, failing if any regress. This is the
  Direction A path — the feature the product is named for — which unit tests
  cannot reach: a missing or wrong FUSE callback only shows up when something
  actually calls it through the kernel.

  Self-contained: starts an agent serving a temp directory and mounts it with the
  Windows client over loopback, so it runs anywhere WinFsp is installed
  (including CI) without WSL.

.EXAMPLE
  .\scripts\fs-conformance.ps1                 # write-through mount
  .\scripts\fs-conformance.ps1 -Writeback      # also the write-back buffering path

  Exit status is the number of failed checks (0 = all good).
#>
[CmdletBinding()]
param(
  [string] $Build = 'build\msvc-release',
  [string] $Drive = '',            # drive letter to use; default: first free of Z..T
  [int]    $Port  = 51998,
  [switch] $Writeback,
  [switch] $TraceInvalidations     # log every invalidation the agent broadcasts (diagnosing a failure)
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$agent = Join-Path $root "$Build\src\tools\wsldrived.exe"
$cli   = Join-Path $root "$Build\src\tools\wsldrive.exe"
foreach ($exe in $agent, $cli) { if (-not (Test-Path $exe)) { Write-Host "not built: $exe"; exit 99 } }

# Candidate letters. Test-Path cannot see every reservation: a disconnected
# network mapping still owns its letter, and even when WinFsp manages to mount
# over one, Git for Windows resolves the letter through the stale mapping. So
# skip letters with a persistent mapping (HKCU:\Network), and still attempt the
# mount on each remaining candidate in turn until one takes.
$mapped = @(Get-ChildItem HKCU:\Network -ErrorAction SilentlyContinue | ForEach-Object { $_.PSChildName.ToUpper() })
$letters = if ($Drive) { @($Drive.TrimEnd(':').ToUpper()) }
           else { @('Z','Y','X','V','U','T') | Where-Object { -not (Test-Path "${_}:\") -and $_ -notin $mapped } }
if (-not $letters) { Write-Host 'no free drive letter'; exit 99 }

$scratch = Join-Path ([IO.Path]::GetTempPath()) "wsldrive-conf-$PID"
$work = Join-Path $scratch 'root'          # the served tree
New-Item -ItemType Directory -Force -Path $work | Out-Null
$env:WSLDRIVE_TOKEN = "conformance-$PID"

# --- content the AGENT serves (created before the scan) -------------------------
Set-Content -NoNewline -Path (Join-Path $work 'lt.txt') -Value 'target-content'
Set-Content -NoNewline -Path (Join-Path $work 'héllo wörld.txt') -Value 'unicode' -Encoding utf8
$many = Join-Path $work 'many1500'
New-Item -ItemType Directory -Path $many | Out-Null
1..1500 | ForEach-Object { [IO.File]::WriteAllText((Join-Path $many "f$_.txt"), "$_") }
# A symlink needs a privilege (or Developer Mode) on Windows; optional, reported not gated.
$haveLink = $false
try { New-Item -ItemType SymbolicLink -Path (Join-Path $work 'lnk') -Target 'lt.txt' -ErrorAction Stop | Out-Null; $haveLink = $true } catch {}

$agentProc = $null; $cliProc = $null
function Cleanup {
  if ($script:traceProc -and -not $script:traceProc.HasExited) { Stop-Process -Id $script:traceProc.Id -Force -ErrorAction SilentlyContinue }
  if ($script:cliProc -and -not $script:cliProc.HasExited)   { Stop-Process -Id $script:cliProc.Id -Force -ErrorAction SilentlyContinue }
  if ($script:agentProc -and -not $script:agentProc.HasExited) { Stop-Process -Id $script:agentProc.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 500
  Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}

# ErrorActionPreference is Stop, so anything unexpected outside a Check - a
# missing binary, a path that vanished - terminates the script. Without this
# the drive letter stays mounted and the agent keeps running, and the next run
# then fails to take the letter. `trap` covers the whole body without having to
# wrap and re-indent it.
trap {
  Write-Host "  ERROR (outside a check): $($_.Exception.Message)"
  Cleanup
  exit 97
}

Write-Host "serving $work $(if ($Writeback) { '(write-back)' } else { '(write-through)' })"
$agentProc = Start-Process -FilePath $agent -PassThru -NoNewWindow `
  -ArgumentList @('--root', "`"$work`"", '--listen', "tcp://127.0.0.1:$Port") `
  -RedirectStandardError (Join-Path $scratch 'agent.log')
Start-Sleep -Seconds 2
$M = ''
foreach ($L in $letters) {
  $cliArgs = @('mount', "${L}:", '--connect', "tcp://127.0.0.1:$Port")
  if ($Writeback) { $cliArgs += '--writeback' }
  $cliProc = Start-Process -FilePath $cli -PassThru -NoNewWindow -ArgumentList $cliArgs `
    -RedirectStandardOutput (Join-Path $scratch 'mount.log') -RedirectStandardError (Join-Path $scratch 'mount.err')
  for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-Path "${L}:\") { $M = "${L}:"; break }
    if ($cliProc.HasExited) { break }   # e.g. "mount point in use": try the next letter
  }
  if ($M) { break }
  Write-Host "  (${L}: not usable, trying the next letter)"
}
if (-not $M) {
  Write-Host 'MOUNT FAILED'
  Get-Content (Join-Path $scratch 'mount.log'), (Join-Path $scratch 'mount.err'), (Join-Path $scratch 'agent.log') -ErrorAction SilentlyContinue | Select-Object -Last 15
  Cleanup; exit 98
}
Write-Host "mounted at $M"
$traceProc = $null
if ($TraceInvalidations) {
  # A second, read-only session on the same agent that prints each invalidation
  # batch as it is broadcast - the mount's view of the tree is driven by these.
  $traceProc = Start-Process -FilePath $cli -PassThru -NoNewWindow `
    -ArgumentList @('fetch', '--connect', "tcp://127.0.0.1:$Port", '--watch') `
    -RedirectStandardOutput (Join-Path $scratch 'inval.log') -RedirectStandardError (Join-Path $scratch 'inval.err')
  Start-Sleep -Seconds 1
}

$script:pass = 0; $script:fail = 0
# A check passes when its body runs without error and does not return $false.
# Side-by-side listing of one mount-relative path, as the mount sees it and as
# the backing store does. A bare "condition was false" does not say which of the
# two views was wrong, which is the first thing worth knowing.
function Dump([string]$rel) {
  $m = if ($rel) { Join-Path $M $rel } else { $M }
  $b = if ($rel) { Join-Path $work $rel } else { $work }
  # Report absence as a value, so "gone on both sides" does not read as the two
  # views disagreeing.
  $ml = if (Test-Path -LiteralPath $m) { (Get-ChildItem -Force -LiteralPath $m -EA SilentlyContinue | ForEach-Object Name | Sort-Object) -join ' ' } else { '<absent>' }
  $bl = if (Test-Path -LiteralPath $b) { (Get-ChildItem -Force -LiteralPath $b -EA SilentlyContinue | ForEach-Object Name | Sort-Object) -join ' ' } else { '<absent>' }
  $label = if ($rel) { $rel } else { '<root>' }
  Write-Host "        mount   [$label]: $ml"
  Write-Host "        backing [$label]: $bl"
  if ($ml -ne $bl) { Write-Host "        ^^ mount and backing store disagree here" }
}

# -Dump names mount-relative paths to list from both sides when the check fails.
# Give it the parent of whatever the check touched, plus the path itself when it
# is a directory whose contents are the point.
# -Settle is a condition (not the whole body, which would re-run the mutation)
# polled after a failure. It answers the question a dump cannot: was the mount
# transiently wrong and then right, or is it wrong and staying wrong? A settle
# time close to a cache timeout points at a stale cache; "still wrong" points at
# lost state. #88 has been chased three times without that distinction.
function Check([string]$name, [scriptblock]$body, [string[]]$Dump = @(), [scriptblock]$Settle = $null) {
  try {
    $r = & $body
    if ($r -is [bool] -and -not $r) { throw 'condition was false' }
    $script:pass++; Write-Host "  ok    $name"
  } catch {
    $script:fail++; Write-Host "  FAIL  $name -- $($_.Exception.Message)"
    if ($Settle) {
      $sw = [Diagnostics.Stopwatch]::StartNew()
      $settled = $false
      while ($sw.ElapsedMilliseconds -lt 5000) {
        try { if (& $Settle) { $settled = $true; break } } catch {}
        Start-Sleep -Milliseconds 10
      }
      $sw.Stop()
      if ($settled) { Write-Host "        settled after $($sw.ElapsedMilliseconds) ms - transient, the mount caught up" }
      else          { Write-Host "        still wrong after 5000 ms - not a timing window" }
    }
    foreach ($p in $Dump) { Dump $p }
  }
}
# Something we knowingly do not support: report it, do not fail the run.
function Known([string]$name, [scriptblock]$body) {
  try { $r = & $body; if ($r -is [bool] -and -not $r) { throw 'false' }; Write-Host "  note  $name (now works)" }
  catch { Write-Host "  known $name (unsupported, documented)" }
}
function Raw([string]$p) { [IO.File]::ReadAllText($p) }   # no encoding/newline surprises
function Hash([string]$p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash }

Write-Host '== basic file operations =='
Check 'create + read'          { [IO.File]::WriteAllText("$M\a.txt", 'hello'); (Raw "$M\a.txt") -eq 'hello' }
Check 'append'                 { [IO.File]::AppendAllText("$M\a.txt", ' world'); (Raw "$M\a.txt") -eq 'hello world' }
Check 'stat reports size'      { (Get-Item "$M\a.txt").Length -eq 11 }
Check 'truncate'               { $fs = [IO.File]::Open("$M\a.txt", 'Open', 'ReadWrite'); try { $fs.SetLength(3) } finally { $fs.Dispose() }; (Raw "$M\a.txt") -eq 'hel' }
Check 'mkdir (nested)'         { New-Item -ItemType Directory -Path "$M\d\sub\deeper" | Out-Null; Test-Path "$M\d\sub\deeper" -PathType Container }
Check 'readdir sees entries'   { (Get-ChildItem "$M\" -Name) -contains 'a.txt' -and (Get-ChildItem "$M\d\sub" -Name) -contains 'deeper' }
Check 'rename file'            { Move-Item "$M\a.txt" "$M\d\b.txt"; (Test-Path "$M\d\b.txt") -and -not (Test-Path "$M\a.txt") } -Dump '', 'd' -Settle { (Test-Path "$M\d\b.txt") -and -not (Test-Path "$M\a.txt") }
Check 'rename directory keeps contents' { Rename-Item "$M\d\sub" 'sub2'; Test-Path "$M\d\sub2\deeper" } -Dump 'd', 'd\sub2', 'd\sub' -Settle { Test-Path "$M\d\sub2\deeper" }
Check 'delete file'            { Copy-Item "$M\d\b.txt" "$M\gone.txt"; Remove-Item "$M\gone.txt"; -not (Test-Path "$M\gone.txt") } -Dump ''
Check 'rmdir'                  { Remove-Item "$M\d\sub2\deeper"; -not (Test-Path "$M\d\sub2\deeper") } -Dump 'd\sub2'
Check 'missing file is an error' { try { Raw "$M\nope.txt"; $false } catch [IO.FileNotFoundException] { $true } }
Check 'case-insensitive lookup'  { (Raw "$M\D\B.TXT") -eq 'hel' }

Write-Host '== metadata operations =='
Check 'set + clear ReadOnly (chmod)' { $f = Get-Item "$M\d\b.txt"; $f.Attributes = 'ReadOnly'; (Get-Item "$M\d\b.txt").Attributes = 'Normal'; $true }
Check 'set LastWriteTime (utimens)'  { (Get-Item "$M\d\b.txt").LastWriteTime = Get-Date; $true }
Check 'create an empty file (touch)' { New-Item -ItemType File -Path "$M\new.txt" | Out-Null; (Get-Item "$M\new.txt").Length -eq 0 }

Write-Host '== changes made behind the mount''s back (watcher) =='
Check 'external dir rename shows contents' {
  New-Item -ItemType Directory -Path (Join-Path $work 'ext\inner') | Out-Null
  [IO.File]::WriteAllText((Join-Path $work 'ext\inner\f'), 'x')
  Start-Sleep -Milliseconds 500
  Rename-Item (Join-Path $work 'ext') 'ext2'
  $seen = $false
  for ($i = 0; $i -lt 50 -and -not $seen; $i++) { Start-Sleep -Milliseconds 100; $seen = Test-Path "$M\ext2\inner\f" }
  $seen
}

Write-Host '== names =='
Check 'unicode name round-trips'  { (Get-ChildItem "$M\" -Name) -contains 'héllo wörld.txt' -and (Raw "$M\héllo wörld.txt") -eq 'unicode' }
Check 'large directory (1500)'    { (Get-ChildItem "$M\many1500").Count -eq 1500 -and (Raw "$M\many1500\f1500.txt") -eq '1500' }

Write-Host '== data integrity =='
$big = New-Object byte[] (1MB); (New-Object Random 7).NextBytes($big)
Check '1 MiB round-trip'         { [IO.File]::WriteAllBytes("$M\big.bin", $big); (Get-Item "$M\big.bin").Length -eq 1MB }
Check 'content matches backing'  { (Hash "$M\big.bin") -eq (Hash (Join-Path $work 'big.bin')) }
Check '200 small files'          { New-Item -ItemType Directory "$M\many" | Out-Null; 1..200 | ForEach-Object { [IO.File]::WriteAllText("$M\many\f$_", "$_") }; (Get-ChildItem "$M\many").Count -eq 200 }
Check 'seek + partial read'      { $fs = [IO.File]::OpenRead("$M\d\b.txt"); try { $fs.Seek(1, 'Begin') | Out-Null; [char]$fs.ReadByte() -eq 'e' } finally { $fs.Dispose() } }
Check 'overwrite in the middle'  { $fs = [IO.File]::Open("$M\d\b.txt", 'Open', 'ReadWrite'); try { $fs.Seek(1, 'Begin') | Out-Null; $fs.WriteByte([byte][char]'X') } finally { $fs.Dispose() }; (Raw "$M\d\b.txt") -eq 'hXl' }
if ($Writeback) {
  Write-Host '== write-back buffering =='
  # Non-contiguous writes through one handle force a flush + restart of the buffer.
  Check 'non-contiguous writes'  { $fs = [IO.File]::Create("$M\wb.bin"); try { $fs.Write([byte[]](1,2,3,4), 0, 4); $fs.Seek(100, 'Begin') | Out-Null; $fs.Write([byte[]](9,9), 0, 2) } finally { $fs.Dispose() }; $b = [IO.File]::ReadAllBytes("$M\wb.bin"); $b.Length -eq 102 -and $b[0] -eq 1 -and $b[3] -eq 4 -and $b[100] -eq 9 -and $b[101] -eq 9 }
  # Past the 8 MiB cap the buffer must flush mid-stream and the result still match.
  $huge = New-Object byte[] (10MB); (New-Object Random 11).NextBytes($huge)
  Check 'stream past the 8 MiB cap' { [IO.File]::WriteAllBytes("$M\huge.bin", $huge); (Hash "$M\huge.bin") -eq (Hash (Join-Path $work 'huge.bin')) -and (Get-Item "$M\huge.bin").Length -eq 10MB }
  Check 'read after buffered write sees the bytes' { $fs = [IO.File]::Open("$M\rw.txt", 'Create', 'ReadWrite'); try { $w = [Text.Encoding]::ASCII.GetBytes('buffered'); $fs.Write($w, 0, $w.Length); $fs.Flush(); $fs.Seek(0, 'Begin') | Out-Null; $r = New-Object byte[] 8; $fs.Read($r, 0, 8) | Out-Null; [Text.Encoding]::ASCII.GetString($r) -eq 'buffered' } finally { $fs.Dispose() } }
}

Write-Host '== a multi-step tool workload (git exercises many ops at once) =='
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
  $g = @('-c', 'safe.directory=*', '-c', 'user.email=a@b', '-c', 'user.name=c')
  Check 'git init'          { $o = & git @g init -q "$M\repo" 2>&1; if ($LASTEXITCODE -ne 0) { throw "rc=$LASTEXITCODE $o" }; Test-Path "$M\repo\.git\HEAD" }
  Check 'git add + commit'  { Push-Location "$M\repo"; try { 'x' | Set-Content f.txt; $o = & git @g add . 2>&1; if ($LASTEXITCODE -ne 0) { throw "add rc=$LASTEXITCODE $o" }; $o = & git @g commit -qm first 2>&1; if ($LASTEXITCODE -ne 0) { throw "commit rc=$LASTEXITCODE $o" }; $true } finally { Pop-Location } }
  Check 'git status clean'  { Push-Location "$M\repo"; try { $s = & git @g status --porcelain 2>&1; $LASTEXITCODE -eq 0 -and -not $s } finally { Pop-Location } }
  Check 'git log reads back' { Push-Location "$M\repo"; try { $l = & git @g log --oneline 2>&1; $LASTEXITCODE -eq 0 -and "$l" -match 'first' } finally { Pop-Location } }
  Check 'git checkout -b + commit' { Push-Location "$M\repo"; try { & git @g checkout -qb feature 2>&1 | Out-Null; 'y' | Add-Content f.txt; & git @g add . 2>&1 | Out-Null; & git @g commit -qm second 2>&1 | Out-Null; $LASTEXITCODE -eq 0 } finally { Pop-Location } }
} else { Write-Host '  skip  git not available' }

Write-Host '== symlinks =='
if ($haveLink) {
  Known 'readlink through WinFsp' { (Get-Item "$M\lnk").Target -match 'lt\.txt' }
} else { Write-Host '  skip  cannot create a symlink on the backing store (needs privilege / Developer Mode)' }
Known 'creating a symlink on the mount' { New-Item -ItemType SymbolicLink -Path "$M\d\link" -Target 'b.txt' | Out-Null; $true }

Write-Host ''
Write-Host "passed: $script:pass   failed: $script:fail"
if ($script:fail -gt 0) {
  Start-Sleep -Milliseconds 500
  Write-Host '--- what the mount shows for repo\.git vs the backing store ---'
  Write-Host "  mount:   $((Get-ChildItem -Force "$M\repo\.git" -Name -ErrorAction SilentlyContinue) -join ' ')"
  Write-Host "  backing: $((Get-ChildItem -Force (Join-Path $work 'repo\.git') -Name -ErrorAction SilentlyContinue) -join ' ')"
  Write-Host "  mount root: $((Get-ChildItem -Force "$M\" -Name -ErrorAction SilentlyContinue) -join ' ')"
  Write-Host '--- mount stderr ---'
  Get-Content (Join-Path $scratch 'mount.err') -ErrorAction SilentlyContinue | Select-Object -Last 10
}
if ($TraceInvalidations) {
  Write-Host '--- invalidations broadcast by the agent (batches, rescans, repo paths) ---'
  Get-Content (Join-Path $scratch 'inval.log') -ErrorAction SilentlyContinue | Select-String -Pattern '^\[gen|rescan|repo' | ForEach-Object { $_.Line }
}
Cleanup
exit $script:fail
