<#
.SYNOPSIS
  Interactive installer for wsldrive. Gathers and CONFIRMS everything it needs
  (drive letter, distro, paths, whether to restart WSL), then applies it so that
  after a reboot the drive(s) are mounted automatically with no manual tweaking.

  What it sets up:
    * installs the wsldrive binaries into Program Files
    * checks for WinFsp (Direction A) and offers to chain-install it
    * registers the Hyper-V socket service GUIDs (the fast transport)
    * restarts WSL once (with your confirmation) so it picks up the registration
    * registers an at-logon scheduled task that mounts the drive(s) on every boot
      and re-discovers the dynamic WSL VM GUID each time

  RUN ELEVATED (Administrator). Re-runnable / idempotent.

.EXAMPLE
  # interactive (recommended): asks and confirms each choice
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1

.EXAMPLE
  # unattended: Direction A, map Ubuntu ~ to W: over hvsocket
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Unattended `
    -DriveLetter W -Distro Ubuntu -WslRoot '~' -Yes

.EXAMPLE
  # several mounts at once (multiple distros) from a config file
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Config wsldrive.json

  # wsldrive.json (see wsldrive.example.json):
  #   { "mounts": [
  #       { "drive": "W", "distro": "Ubuntu", "wslRoot": "~" },
  #       { "drive": "Y", "distro": "Debian", "wslRoot": "~/work" },
  #       { "winRoot": "C:/projects", "mountpoint": "~/win", "distro": "Ubuntu" }
  #   ] }
  # Each mount gets its own logon task and its own port (auto-assigned).

.EXAMPLE
  # preview only, change nothing
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -DryRun

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
  [switch] $Unattended,                       # no prompts; use the values below / defaults
  [switch] $Advanced,                         # also offer Direction B (mount a Windows folder in WSL)
  [switch] $Yes,                              # auto-confirm destructive steps (WSL restart)
  [switch] $DryRun,                           # print the plan, change nothing
  [switch] $Uninstall,                        # remove tasks + binaries + registration

  [string] $InstallDir = "$env:ProgramFiles\wsldrive",
  [string] $BinDir,                           # source of built binaries (auto-detected)
  [string] $Config,                           # JSON file defining several mounts (see .EXAMPLE)

  # Direction A: mount a WSL ext4 tree as a Windows drive letter
  [string] $DriveLetter,                      # e.g. W  (empty in -Unattended => skip Direction A)
  [string] $Distro,                           # WSL distro (default: the WSL default distro)
  [string] $WslRoot = '~',                    # path inside the distro to serve
  [string] $LinuxAgent,                       # Linux wsldrived (agent) to stage into WSL (Direction A)

  # Direction B: mount a Windows path inside WSL
  [string] $WinRoot,                          # e.g. C:\projects  (empty => skip Direction B)
  [string] $Mountpoint = '~/win',             # mount point inside the distro
  [string] $LinuxBin,                         # Linux wsldrive (client) to stage into WSL (Direction B)

  [switch] $NoHvsocket,                       # use loopback TCP instead of Hyper-V sockets
  [switch] $NoShutdown,                        # never run `wsl --shutdown` (installer drives this)
  [string] $WinFspMsi,                        # path to WinFsp MSI to chain-install if missing
  [int]    $FirstPort = 5700,
  [int]    $PortCount = 10
)

$ErrorActionPreference = 'Stop'
$env:WSL_UTF8 = '1'   # make wsl.exe emit UTF-8 instead of UTF-16
$root = Split-Path -Parent $PSScriptRoot
$TaskPrefix = 'wsldrive-mount-'   # every mount's logon task starts with this
$BasePortA  = 51789               # Direction A (loopback TCP); one port per mount

# ---- tiny UI helpers -------------------------------------------------------
function Say  ($m) { Write-Host $m }
function Ok   ($m) { Write-Host "[ok]   $m"   -ForegroundColor Green }
function Warn ($m) { Write-Host "[warn] $m"   -ForegroundColor Yellow }
function Step ($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Plan ($m) { if ($DryRun) { Write-Host "[dry]  would $m" -ForegroundColor DarkGray } else { Write-Host "       $m" } }

function Ask([string]$prompt, [string]$default) {
  if ($Unattended) { return $default }
  $suffix = if ($default) { " [$default]" } else { "" }
  $a = Read-Host "$prompt$suffix"
  if ([string]::IsNullOrWhiteSpace($a)) { return $default } else { return $a.Trim() }
}
function AskYN([string]$prompt, [bool]$default) {
  if ($Unattended) { return $default }
  $d = if ($default) { 'Y/n' } else { 'y/N' }
  while ($true) {
    $a = (Read-Host "$prompt [$d]").Trim().ToLower()
    if ($a -eq '')             { return $default }
    if ($a -in 'y','yes')      { return $true }
    if ($a -in 'n','no')       { return $false }
  }
}

# Every wsldrive logon task currently registered (any mount, any older naming).
# Defined up here on purpose: PowerShell resolves functions as the script runs,
# and the uninstall path below calls this long before the rest of the helpers.
function Get-WsldriveTasks {
  @(Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -like "$TaskPrefix*" })
}

# ---- environment probes ----------------------------------------------------
function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Test-WinFsp {
  $k = 'HKLM:\SOFTWARE\WOW6432Node\WinFsp'
  (Test-Path $k) -and ($null -ne (Get-ItemProperty -Path $k -Name 'InstallDir' -ErrorAction SilentlyContinue).InstallDir)
}
function Get-DefaultDistro {
  try { $d = (& wsl.exe --status 2>$null | Select-String 'Default Distribution:').ToString() }
  catch { $d = $null }
  if ($d) { return ($d -replace '.*:\s*', '').Trim() }
  try { return ((& wsl.exe -l -q 2>$null) | Where-Object { $_.Trim() } | Select-Object -First 1).Trim() }
  catch { return '' }
}
function Get-FreeDriveLetter {
  foreach ($c in 'W','X','Y','Z','V','U','T') {
    if (-not (Test-Path "${c}:")) { return $c }
  }
  return 'W'
}
# Windows path -> the /mnt/<drive>/... path a WSL process sees.
function To-WslPath([string]$winPath) {
  $p = (Resolve-Path -LiteralPath $winPath -ErrorAction SilentlyContinue)
  $full = if ($p) { $p.Path } else { $winPath }
  if ($full -match '^([A-Za-z]):[\\/](.*)$') {
    return '/mnt/' + $Matches[1].ToLower() + '/' + ($Matches[2] -replace '\\','/')
  }
  return $full
}

# Keep a log: the GUI installer runs this in a console that closes on exit, so
# without one an error message is gone before anyone can read it.
$LogPath = Join-Path $env:TEMP 'wsldrive-install.log'
try { Start-Transcript -Path $LogPath -Append -ErrorAction Stop | Out-Null } catch {}

# ===========================================================================
Step 'wsldrive installer'

# --- argument quoting -------------------------------------------------------
# Quote one argument for a Windows command line, by CommandLineToArgvW rules:
# backslashes are literal except before a quote, so any run of them that ends
# up adjacent to the closing quote has to be doubled. Without this a value
# containing a quote ends the argument early, and an InstallDir ending in a
# backslash escapes the closing quote and swallows the next argument.
function ConvertTo-WinArg([string] $s) {
  $s = $s -replace '(\\*)"', '$1$1\"'
  $s = $s -replace '(\\+)$', '$1$1'
  return '"' + $s + '"'
}

# Quote one value for /bin/sh. Anything may appear inside single quotes except
# a single quote, which is closed, escaped and reopened.
function ConvertTo-ShQuoted([string] $s) { "'" + ($s -replace "'", "'\'''") + "'" }

# Same, for a path that may start with ~. A leading ~ has to stay OUTSIDE the
# quotes or the shell stops treating it as a home reference - and ~ is the
# default mountpoint. Uses ${HOME} rather than a double-quoted expansion,
# because this ends up inside an already double-quoted Windows argument.
function ConvertTo-ShPath([string] $s) {
  if ($s -eq '~') { return '${HOME}' }
  if ($s.StartsWith('~/')) { return '${HOME}/' + (ConvertTo-ShQuoted $s.Substring(2)) }
  return ConvertTo-ShQuoted $s
}

if (-not $DryRun -and -not (Test-Admin)) {
  Warn 'This installer needs Administrator rights (driver, HKLM registry, scheduled task).'
  Warn 'Relaunching elevated...'
  # Rebuild the original invocation (named + switch params) so nothing is lost on relaunch.
  #
  # The elevated process does not inherit this working directory, so a relative
  # path on the command line no longer resolves over there - which is what made
  # the README's own `-Config wsldrive.json` fail with "Config file not found"
  # the moment it relaunched. Make the Windows-side path arguments absolute
  # first. WslRoot and Mountpoint are deliberately NOT in this list: those are
  # paths inside the distro, not on this filesystem.
  $pathParams = @('InstallDir','BinDir','Config','LinuxAgent','WinRoot','LinuxBin','WinFspMsi')
  $argline = @('-ExecutionPolicy','Bypass','-File',(ConvertTo-WinArg $PSCommandPath))
  foreach ($kv in $PSBoundParameters.GetEnumerator()) {
    if ($kv.Value -is [switch]) {
      if ($kv.Value.IsPresent) { $argline += "-$($kv.Key)" }
      continue
    }
    $v = [string]$kv.Value
    if ($v -and $pathParams -contains $kv.Key) {
      $v = try { (Resolve-Path -LiteralPath $v -ErrorAction Stop).Path }
           catch { [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).Path, $v)) }
    }
    $argline += "-$($kv.Key)"
    $argline += (ConvertTo-WinArg $v)
  }
  $child = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argline -PassThru
  # Wait, and pass the child's status on. Exiting 0 immediately told a scripted
  # caller the install had succeeded before it had even begun.
  $child.WaitForExit()
  exit $child.ExitCode
}
if ($DryRun) { Warn 'DRY RUN — nothing will be changed.' }

# ---- uninstall path --------------------------------------------------------
if ($Uninstall) {
  Step 'Uninstalling'
  $existing = Get-WsldriveTasks
  if ($existing.Count -eq 0) { Say '       (no wsldrive mount tasks registered)' }
  # The distros we staged binaries into are recorded in the tasks themselves
  # (`--distro X` for Direction A, `-d X` for Direction B); read them before the
  # tasks go away so the staged files can be removed too.
  $distros = @()
  foreach ($t in $existing) {
    $args = ($t.Actions | ForEach-Object { $_.Arguments }) -join ' '
    if ($args -match '(?:--distro|-d)\s+"?([^\s"]+)') { $distros += $Matches[1] }
  }
  $distros = @($distros | Select-Object -Unique)
  foreach ($t in $existing) {
    Plan "stop + remove scheduled task $($t.TaskName)"
    if (-not $DryRun) {
      Stop-ScheduledTask -TaskName $t.TaskName -ErrorAction SilentlyContinue
      Unregister-ScheduledTask -TaskName $t.TaskName -Confirm:$false
    }
  }
  # A live mount holds its own .exe open; end it or the install dir cannot be
  # removed (and the drive would stay mounted).
  if (Get-Process wsldrive,wsldrivew -ErrorAction SilentlyContinue) {
    Plan 'end running wsldrive / wsldrivew processes (unmounts their drives)'
    if (-not $DryRun) {
      Get-Process wsldrive,wsldrivew -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
      Start-Sleep -Milliseconds 800
    }
  }
  Plan "unregister hvsocket service GUIDs (ports $FirstPort..$($FirstPort+$PortCount-1))"
  if (-not $DryRun) { & "$PSScriptRoot\register-hvsocket.ps1" -FirstPort $FirstPort -Count $PortCount -Unregister | Out-Null }
  foreach ($d in $distros) {
    # The agent exits on its own when its client goes away, but not instantly;
    # a fresh install right after this would race it for the port.
    Plan "end the wsldrive agent/client in distro $d and remove staged ~/.local/bin/wsldrive and wsldrived"
    if (-not $DryRun) { & wsl.exe -d $d -- sh -c 'pkill -x wsldrived; pkill -x wsldrive; rm -f ~/.local/bin/wsldrive ~/.local/bin/wsldrived' 2>$null | Out-Null }
  }
  if (Test-Path "$env:LOCALAPPDATA\wsldrive") {
    Plan "delete $env:LOCALAPPDATA\wsldrive (staging copies)"
    if (-not $DryRun) { Remove-Item -Recurse -Force "$env:LOCALAPPDATA\wsldrive" -ErrorAction SilentlyContinue }
  }
  if (Test-Path $InstallDir) {
    Plan "delete $InstallDir"
    if (-not $DryRun) {
      # Something may still hold a file for a moment after the processes ended.
      for ($try = 1; ; $try++) {
        try { Remove-Item -Recurse -Force $InstallDir -ErrorAction Stop; break }
        catch { if ($try -ge 5) { Warn "Could not remove $InstallDir completely: $($_.Exception.Message)"; break }; Start-Sleep -Milliseconds 700 }
      }
    }
  }
  Ok 'Uninstalled. (WinFsp was left installed; remove it from Apps if you no longer need it.)'
  exit 0
}

# ---- locate the built binaries --------------------------------------------
# Binaries are found in a source tree (build/... after scripts\build.ps1) or in a
# release download, where they sit next to the scripts folder. Returns the first
# path that exists.
function First-Existing([string[]]$candidates) {
  foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
  return ''
}
if (-not $BinDir) {
  foreach ($c in 'msvc-release','msvc-relwithdebinfo','msvc-debug') {
    $p = Join-Path $root "build\$c\src\tools"
    if (Test-Path (Join-Path $p 'wsldrive.exe')) { $BinDir = $p; break }
  }
  # Release layout: wsldrive.exe beside scripts\install.ps1 (or next to it).
  if (-not $BinDir) { $BinDir = First-Existing @($root, $PSScriptRoot | Where-Object { Test-Path (Join-Path $_ 'wsldrive.exe') }) }
}
$srcCli      = if ($BinDir) { Join-Path $BinDir 'wsldrive.exe' }  else { '' }
$srcAgent    = if ($BinDir) { Join-Path $BinDir 'wsldrived.exe' } else { '' }
$srcLauncher = if ($BinDir) { Join-Path $BinDir 'wsldrivew.exe' } else { '' }  # windowless launcher (optional)
if (-not $srcCli -or -not (Test-Path $srcCli) -or -not (Test-Path $srcAgent)) {
  throw "Could not find wsldrive.exe / wsldrived.exe. Build first (.\scripts\build.ps1), unpack the release zip, or pass -BinDir."
}
# Linux binaries: a source build, or the names used by the release zip and by the
# GUI installer's install directory.
# Looked for beside the scripts folder ($root, the release zip and the GUI's
# install dir) and beside this script itself, in case someone moved it.
$linuxCli = if ($LinuxBin) { $LinuxBin } else {
  First-Existing @((Join-Path $root 'build\linux-release\src\tools\wsldrive'),
                   (Join-Path $root 'wsldrive-linux-x64'), (Join-Path $root 'wsldrive-linux'),
                   (Join-Path $PSScriptRoot 'wsldrive-linux-x64'), (Join-Path $PSScriptRoot 'wsldrive-linux'))
}
$linuxAgent = if ($LinuxAgent) { $LinuxAgent } else {
  First-Existing @((Join-Path $root 'build\linux-release\src\tools\wsldrived'),
                   (Join-Path $root 'wsldrived-linux-x64'), (Join-Path $root 'wsldrived-linux'),
                   (Join-Path $PSScriptRoot 'wsldrived-linux-x64'), (Join-Path $PSScriptRoot 'wsldrived-linux'))
}

# Absolute $HOME inside a distro (cached — each distro is asked at most once).
$script:WslHomeCache = @{}
function Get-WslHome([string]$d) {
  if ([string]::IsNullOrWhiteSpace($d)) { return '' }
  if ($script:WslHomeCache.ContainsKey($d)) { return $script:WslHomeCache[$d] }
  $h = ''
  # A plain sh, not a login shell: a login shell may print a motd/banner first,
  # and not every distro has bash (Alpine).
  try { $h = (& wsl.exe -d $d -- sh -c 'echo "$HOME"' 2>$null | Select-Object -First 1) } catch { $h = '' }
  if ($h) { $h = $h.Trim() }
  # wsl.exe reports "there is no distribution with the supplied name" on STDOUT,
  # so anything that is not an absolute Linux path is an error message, not a home.
  if ($h -notmatch '^/') { $h = '' }
  $script:WslHomeCache[$d] = $h
  return $h
}
# The distros wsl.exe knows about, for an error message.
function Get-WslDistros {
  try { @((& wsl.exe -l -q 2>$null) | ForEach-Object { $_.Trim() } | Where-Object { $_ }) } catch { @() }
}

# Expand a leading ~ against a distro's home. The path is single-quoted when
# passed to the agent, so ~ would not expand in the distro shell and the agent
# would try to open a literal "~" directory and exit.
function Expand-WslPath([string]$p, [string]$wslHomeDir) {
  if (-not $wslHomeDir) { return $p }
  if ($p -eq '~') { return $wslHomeDir }
  if ($p -like '~/*') { return $wslHomeDir + $p.Substring(1) }
  return $p
}

# Copy a Windows-side Linux binary into a distro at an absolute path under that
# distro's home, and return the WSL path it was placed at.
function Stage-Into-Wsl([string]$srcWin, [string]$name, [string]$d) {
  $hm = Get-WslHome $d
  if (-not $hm) { return '' }
  $staged = "$env:LOCALAPPDATA\wsldrive\$name"
  New-Item -ItemType Directory -Force -Path (Split-Path $staged) | Out-Null
  Copy-Item -Force $srcWin $staged
  $srcWsl = To-WslPath $staged
  $dest = "$hm/.local/bin/$name"
  & wsl.exe -d $d -- sh -c "mkdir -p '$hm/.local/bin' && cp '$srcWsl' '$dest' && chmod +x '$dest'" 2>$null
  if ($LASTEXITCODE -ne 0) {
    # Silently returning a path here would register a task pointing at a binary
    # that is not there, and the mount would never come up.
    throw "Could not copy $name into ${d}:$dest (is /mnt/c mounted in the distro? exit $LASTEXITCODE)."
  }
  return $dest
}

# Direction B runs libfuse3 inside the distro; without the fuse3 package the
# staged client cannot mount anything, and the failure would only show at logon.
function Test-WslFuse3([string]$d) {
  & wsl.exe -d $d -- sh -c 'command -v fusermount3 >/dev/null 2>&1 && [ -e /usr/lib/x86_64-linux-gnu/libfuse3.so.3 -o -e /usr/lib/libfuse3.so.3 -o -e /lib/x86_64-linux-gnu/libfuse3.so.3 ]' 2>$null
  return ($LASTEXITCODE -eq 0)
}

# A stable, filename-safe task name per mount, so several mounts coexist.
function Get-TaskName($m) {
  if ($m.Direction -eq 'A') { return "$TaskPrefix$($m.Drive)" }
  # Name Direction B by its mountpoint's last component, which is what the user
  # recognizes (~/win -> "win"); collisions are caught by validation.
  $leaf = @($m.Mountpoint -split '[\\/]' | Where-Object { $_ }) | Select-Object -Last 1
  $slug = ("$leaf" -replace '[^A-Za-z0-9]+','-').Trim('-')
  if (-not $slug) { $slug = 'b' }
  return "$TaskPrefix$($m.Distro)-$slug".ToLower()
}

# ===========================================================================
# 1. Gather + confirm configuration
# ===========================================================================
Step 'Configuration'

$distro = if ($Distro) { $Distro } else { Get-DefaultDistro }

# Direction B transport: measure, do not guess.
#
# hvsocket exists because the WSL localhost relay used to cost seconds per
# round-trip. On current WSL it costs a fraction of a millisecond, and where
# that holds, plain TCP is the better choice - it needs no HKLM registration,
# no `wsl --shutdown`, and no elevated logon task, so the write-capable Windows
# agent stops running as Administrator.
#
# This cannot be decided from the installed WSL version. A running WSL2 VM
# keeps the kernel it booted with, so two machines on identical WSL versions
# behave differently depending on whether the VM has restarted since its
# kernel updated. So probe the thing that actually varies.
if ($PSBoundParameters.ContainsKey('NoHvsocket')) {
  $useHv = -not $NoHvsocket        # explicit wins, always
} else {
  # $srcCli is the built wsldrive.exe this install will copy; probe with it.
  $probe = $srcCli
  if (Test-Path $probe) {
    $rtt = & $probe probe-transport --distro $distro 2>$null
    $fast = ($LASTEXITCODE -eq 0)
    if ($LASTEXITCODE -eq 2 -or -not $rtt) {
      $useHv = $true
      Warn "Could not measure the loopback round-trip; using hvsocket for Direction B."
    } else {
      $useHv = -not $fast
      Say "       loopback round-trip $rtt ms -> Direction B over $(if($useHv){'hvsocket'}else{'TCP'})"
    }
  } else {
    $useHv = $true                 # cannot probe without the binary; keep the safe default
  }
}

# Every mount is one entry here, so several drives / distros coexist: each gets
# its own port and its own logon task.
$mounts = @()
function Prop($o, [string]$name, $default) {
  if ($o.PSObject.Properties.Name -contains $name -and $null -ne $o.$name -and "$($o.$name)" -ne '') { return $o.$name }
  return $default
}

if ($Config) {
  # ---- mounts from a config file ----
  if (-not (Test-Path $Config)) { throw "Config file not found: $Config" }
  $cfg = Get-Content -Raw $Config | ConvertFrom-Json
  if (-not $cfg.mounts) { throw "Config has no 'mounts' array: $Config" }
  foreach ($m in $cfg.mounts) {
    $d = Prop $m 'distro' $distro
    if (-not $d) { $d = Get-DefaultDistro }
    $drv = Prop $m 'drive' ''
    $wr  = Prop $m 'winRoot' ''
    $dir = Prop $m 'direction' $(if ($wr) { 'B' } else { 'A' })   # inferred when omitted
    if ($dir -eq 'A') {
      if (-not $drv) { throw "Config mount is Direction A but has no 'drive': $($m | ConvertTo-Json -Compress)" }
      $mounts += [pscustomobject]@{
        Direction = 'A'; Distro = $d
        Drive     = ($drv -replace ':','').ToUpper()
        WslRoot   = (Prop $m 'wslRoot' '~')
        Port      = [int](Prop $m 'port' 0)      # 0 = auto-assign
        UseHv     = $false                        # Direction A always loopback TCP
        Mountpoint = ''; WinRoot = ''
      }
    } else {
      if (-not $wr) { throw "Config mount is Direction B but has no 'winRoot': $($m | ConvertTo-Json -Compress)" }
      $mounts += [pscustomobject]@{
        Direction = 'B'; Distro = $d
        WinRoot   = $wr
        Mountpoint = (Prop $m 'mountpoint' '~/win')
        Port      = [int](Prop $m 'port' 0)
        UseHv     = [bool](Prop $m 'hvsocket' $useHv)
        Drive = ''; WslRoot = ''
      }
    }
  }
} else {
  # ---- single mount from parameters / prompts (easy mode) ----
  $doA = $false; $letter = ''
  if ($Unattended) {
    $doA = [bool]$DriveLetter
    $letter = ($DriveLetter -replace ':','').ToUpper()
  } else {
    $doA = AskYN 'Mount a WSL folder as a Windows drive letter (Direction A)?' $true
    if ($doA) {
      $defLetter = if ($DriveLetter) { ($DriveLetter -replace ':','').ToUpper() } else { Get-FreeDriveLetter }
      $letter = (Ask 'Drive letter' $defLetter).TrimEnd(':').ToUpper()
      $distro = Ask 'WSL distro to serve from' $distro
      $WslRoot = Ask 'Folder inside the distro to expose (~ = home, / = whole distro)' $WslRoot
    }
  }
  if ($doA) {
    $mounts += [pscustomobject]@{ Direction='A'; Distro=$distro; Drive=$letter; WslRoot=$WslRoot
                                  Port=0; UseHv=$false; Mountpoint=''; WinRoot='' }
  }

  # Direction B (advanced): mounts a Windows folder *inside WSL* (a Linux path,
  # not a drive letter). Only offered in advanced mode; easy mode is A only.
  $doB = $false
  if ($Unattended) {
    $doB = [bool]$WinRoot   # passing -WinRoot opts into Direction B
  } elseif ($Advanced -or $WinRoot) {
    $doB = AskYN 'Advanced: also mount a Windows folder inside WSL (Direction B)?' ([bool]$WinRoot)
    if ($doB) {
      $WinRoot = Ask 'Windows folder to expose (e.g. C:\projects)' $WinRoot
      $Mountpoint = Ask 'Mount point inside the distro' $Mountpoint
      if (-not $distro) { $distro = Ask 'WSL distro to mount into' (Get-DefaultDistro) }
    }
  }
  if ($doB) {
    $mounts += [pscustomobject]@{ Direction='B'; Distro=$distro; WinRoot=$WinRoot; Mountpoint=$Mountpoint
                                  Port=0; UseHv=$useHv; Drive=''; WslRoot='' }
  }
}

if ($mounts.Count -eq 0) { Warn 'Nothing selected to mount. Exiting.'; exit 0 }

# ---- normalize: resolve ~ per distro, assign ports, derive task names --------
$usedA = @($mounts | Where-Object { $_.Direction -eq 'A' -and $_.Port -gt 0 } | ForEach-Object { $_.Port })
$usedB = @($mounts | Where-Object { $_.Direction -eq 'B' -and $_.Port -gt 0 } | ForEach-Object { $_.Port })
$nextA = $BasePortA
$nextB = $FirstPort
foreach ($m in $mounts) {
  if (-not $m.Distro) { throw 'A mount has no distro and no WSL default distro was found.' }
  $hm = Get-WslHome $m.Distro
  if (-not $hm) {
    # An unknown or stopped distro used to sail through and produce a task that
    # can never mount (and a literal "~" as the served path). Stop here instead;
    # a dry run only reports it, so the rest of the plan is still visible.
    $known = Get-WslDistros
    $msg = "WSL distro '$($m.Distro)' was not found or could not be started. Installed distros: $(if ($known) { $known -join ', ' } else { '(none found - is WSL installed?)' })"
    if ($DryRun) { Warn $msg } else { throw $msg }
  }
  if ($m.Direction -eq 'A') {
    $m.WslRoot = Expand-WslPath $m.WslRoot $hm
    if ($m.Port -le 0) { while ($usedA -contains $nextA) { $nextA++ }; $m.Port = $nextA; $usedA += $nextA; $nextA++ }
  } else {
    $m.Mountpoint = Expand-WslPath $m.Mountpoint $hm
    if ($m.Port -le 0) { while ($usedB -contains $nextB) { $nextB++ }; $m.Port = $nextB; $usedB += $nextB; $nextB++ }
  }
  $m | Add-Member -NotePropertyName TaskName -NotePropertyValue (Get-TaskName $m) -Force
  $m | Add-Member -NotePropertyName WslHome  -NotePropertyValue $hm -Force
}

# ---- validate: nothing may collide -----------------------------------------
$dupDrive = @($mounts | Where-Object { $_.Direction -eq 'A' } | Group-Object Drive | Where-Object { $_.Count -gt 1 })
if ($dupDrive) { throw "Two mounts want the same drive letter: $(($dupDrive | ForEach-Object { $_.Name }) -join ', ')" }
$dupPort = @($mounts | Group-Object Port | Where-Object { $_.Count -gt 1 })
if ($dupPort) { throw "Two mounts want the same port: $(($dupPort | ForEach-Object { $_.Name }) -join ', ')" }
$dupTask = @($mounts | Group-Object TaskName | Where-Object { $_.Count -gt 1 })
if ($dupTask) { throw "Two mounts resolve to the same task name: $(($dupTask | ForEach-Object { $_.Name }) -join ', ')" }
foreach ($m in @($mounts | Where-Object { $_.Direction -eq 'A' })) {
  if ($m.Drive -notmatch '^[A-Z]$') { throw "Bad drive letter '$($m.Drive)' (expected a single letter A-Z)." }
}

$doA = @($mounts | Where-Object { $_.Direction -eq 'A' }).Count -gt 0
$doB = @($mounts | Where-Object { $_.Direction -eq 'B' }).Count -gt 0

# A dry run keeps going past a missing prerequisite (it changes nothing and the
# point is to see the whole plan); a real run stops unless told otherwise.
if ($doA -and -not ($linuxAgent -and (Test-Path $linuxAgent))) {
  Warn "Direction A needs the Linux build of the agent (wsldrived) at:`n         $linuxAgent"
  Warn "Build it in WSL:  cmake --preset linux-release && cmake --build --preset linux-release"
  if (-not $DryRun -and -not (AskYN 'Continue anyway (the task will fail until it exists)?' $false)) { exit 1 }
}
if ($doB -and -not ($linuxCli -and (Test-Path $linuxCli))) {
  Warn "Direction B needs the Linux build of wsldrive at:`n         $linuxCli"
  Warn "Build it in WSL:  cmake --preset linux-release && cmake --build --preset linux-release"
  if (-not $DryRun -and -not (AskYN 'Continue anyway (the task will fail until it exists)?' $false)) { exit 1 }
}

# WSL restart is only needed the first time we register hvsocket services, which
# only happens for Direction B (Direction A uses loopback TCP).
$needShutdown = $doB -and @($mounts | Where-Object { $_.Direction -eq 'B' -and $_.UseHv }).Count -gt 0
$agentWslPath = To-WslPath (Join-Path $InstallDir 'wsldrived.exe')  # Windows agent (Direction B), seen from WSL

# ---- confirmation summary --------------------------------------------------
Step 'Review — this is everything the installer will do'
Say  "  install dir      : $InstallDir"
Say  "  binaries         : $srcCli"
Say  "  mounts           : $($mounts.Count)"
foreach ($m in $mounts) {
  if ($m.Direction -eq 'A') {
    Say "    [A] $($m.Drive):  <=  $($m.Distro) : $($m.WslRoot)   (TCP :$($m.Port), task '$($m.TaskName)')"
  } else {
    Say "    [B] $($m.Distro) : $($m.Mountpoint)  <=  $($m.WinRoot)   ($(if($m.UseHv){"hvsocket :$($m.Port)"}else{"TCP :$($m.Port)"}), task '$($m.TaskName)')"
  }
}
if ($needShutdown) { Say "  hvsocket ports   : $FirstPort..$($FirstPort+$PortCount-1)  (HKLM registration)" }
$stale = @(Get-WsldriveTasks | Where-Object { $mounts.TaskName -notcontains $_.TaskName })
if ($stale.Count -gt 0) {
  Say "  replacing        : $(($stale | ForEach-Object { $_.TaskName }) -join ', ')  (stopped + removed)"
}
if (-not (Test-WinFsp)) { Say "  WinFsp           : NOT installed -> $(if ($WinFspMsi) {'will chain-install'} else {'you will be prompted'})" }
if ($needShutdown) { Say "  WSL restart      : REQUIRED once (closes running WSL sessions) so it picks up the socket registration" }
Say  ""

if (-not $DryRun -and -not $Unattended) {
  if (-not (AskYN 'Proceed with the above?' $true)) { Warn 'Aborted; nothing changed.'; exit 0 }
}

# ===========================================================================
# 2. Apply
# ===========================================================================

# --- WinFsp (Direction A only) ---
if ($doA) {
  Step 'WinFsp'
  if (Test-WinFsp) {
    Ok 'WinFsp runtime already installed.'
  } elseif ($WinFspMsi -and (Test-Path $WinFspMsi)) {
    Plan "chain-install WinFsp from $WinFspMsi"
    if (-not $DryRun) { Start-Process msiexec.exe -Wait -ArgumentList '/i',"`"$WinFspMsi`"",'/qn' }
    if (-not $DryRun -and -not (Test-WinFsp)) { throw 'WinFsp install did not complete.' }
    Ok 'WinFsp installed.'
  } else {
    Warn 'WinFsp is required for Direction A and is not installed.'
    Warn 'Install it from https://winfsp.dev (or re-run with -WinFspMsi <path>), then run this installer again.'
    if ($DryRun) {
      Plan 'stop here until WinFsp is installed (this dry run continues to show the rest of the plan)'
    } elseif (-not (AskYN 'Skip Direction A for now and continue?' $false)) {
      throw 'WinFsp missing.'
    } else {
      $doA = $false
    }
  }
}

# --- stop any running mount ------------------------------------------------
# A live mount holds its own .exe open, so the binaries cannot be replaced while
# it runs. Stop every wsldrive mount first; they are (re-)started at the end.
$running = Get-WsldriveTasks
if ($running.Count -gt 0 -or (Get-Process wsldrive,wsldrivew -ErrorAction SilentlyContinue)) {
  Step 'Stop running mounts'
  foreach ($t in $running) {
    Plan "stop task $($t.TaskName)"
    if (-not $DryRun) { Stop-ScheduledTask -TaskName $t.TaskName -ErrorAction SilentlyContinue }
  }
  Plan 'end any leftover wsldrive / wsldrivew processes (and the agents in the distros)'
  if (-not $DryRun) {
    Get-Process wsldrive,wsldrivew -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    # A previous agent still winding down would hold the loopback port the new
    # mount is about to use, and the first start would fail.
    foreach ($d in @($mounts | ForEach-Object { $_.Distro } | Select-Object -Unique)) {
      & wsl.exe -d $d -- sh -c 'pkill -x wsldrived; pkill -x wsldrive; true' 2>$null | Out-Null
    }
    Start-Sleep -Milliseconds 800   # let the file handles drop
    Warn 'Mounted drives were unmounted so the binaries could be replaced; they are re-started below.'
  }
}

# --- copy binaries (a no-op when a packaged installer already placed them here) ---
Step 'Install binaries'
$sameDir = (Test-Path $InstallDir) -and ((Resolve-Path $BinDir).Path -eq (Resolve-Path $InstallDir).Path)
if ($sameDir) {
  Ok "Binaries already in place ($InstallDir)."
} else {
  Plan "copy wsldrive.exe / wsldrived.exe / wsldrivew.exe -> $InstallDir"
  if (-not $DryRun) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $toCopy = @($srcCli, $srcAgent)
    if (Test-Path $srcLauncher) { $toCopy += $srcLauncher }
    # A just-terminated process can hold its image briefly; retry the copy.
    for ($try = 1; ; $try++) {
      try { Copy-Item -Force $toCopy $InstallDir; break }
      catch {
        if ($try -ge 5) { throw "Could not replace the binaries in $InstallDir (a mount may still be running): $($_.Exception.Message)" }
        Start-Sleep -Milliseconds 700
      }
    }
  }
}
# Stage the Linux binaries into EACH distro that has a mount: Direction A needs
# the agent (wsldrived) to serve the WSL tree, Direction B needs the client.
$agentInWsl = @{}   # distro -> absolute path of its staged agent (for the A task)
foreach ($d in @($mounts | Where-Object { $_.Direction -eq 'A' } | ForEach-Object { $_.Distro } | Select-Object -Unique)) {
  Plan "stage Linux wsldrived into ${d}:$(Get-WslHome $d)/.local/bin/wsldrived"
  if (-not $DryRun -and $linuxAgent -and (Test-Path $linuxAgent)) { $agentInWsl[$d] = Stage-Into-Wsl $linuxAgent 'wsldrived' $d }
}
foreach ($d in @($mounts | Where-Object { $_.Direction -eq 'B' } | ForEach-Object { $_.Distro } | Select-Object -Unique)) {
  Plan "stage Linux wsldrive into ${d}:$(Get-WslHome $d)/.local/bin/wsldrive"
  if (-not $DryRun -and $linuxCli -and (Test-Path $linuxCli)) { [void](Stage-Into-Wsl $linuxCli 'wsldrive' $d) }
  if (-not $DryRun -and -not (Test-WslFuse3 $d)) {
    Warn "Distro '$d' has no fuse3 runtime; Direction B cannot mount there until you install it:"
    Warn "    wsl -d $d -- sudo apt install fuse3        (Debian/Ubuntu; other distros: the package providing fusermount3)"
  }
}
Ok 'Binaries installed.'

# --- hvsocket registration (Direction B only; Direction A uses loopback TCP) ---
if ($needShutdown) {
  Step 'Hyper-V socket registration'
  Plan "register service GUIDs for ports $FirstPort..$($FirstPort+$PortCount-1)"
  if (-not $DryRun) { & "$PSScriptRoot\register-hvsocket.ps1" -FirstPort $FirstPort -Count $PortCount | Out-Null }
  Ok 'hvsocket services registered.'
}

# --- scheduled tasks (auto-start at logon) ---
# Direction A maps a drive letter, which must be visible to the user's normal
# (non-elevated) apps, so its task runs at the user's normal level (Limited).
# Direction B runs Highest only when it uses hvsocket, which needs Hyper-V admin
# for VM-GUID discovery; over TCP it runs Limited like Direction A.
function Register-MountTask([string]$name, [string]$exe, [string]$argline, [bool]$Elevated) {
  # Run through the windowless launcher so no console window appears at logon.
  # (The mount process runs for the life of the mount; a visible console could be
  # closed by accident, which would unmount the drive.) Fall back to direct exec
  # if the launcher isn't present.
  $launcher = Join-Path $InstallDir 'wsldrivew.exe'
  if (Test-Path $launcher) {
    $taskExe = $launcher
    $taskArg = ('"{0}" {1}' -f $exe, $argline)
  } else {
    $taskExe = $exe
    $taskArg = $argline
  }
  Plan "register logon task '$name' ($(if($Elevated){'elevated'}else{'normal'}), windowless): $taskExe $taskArg"
  if ($DryRun) { return }
  $action    = New-ScheduledTaskAction -Execute $taskExe -Argument $taskArg
  $trigger   = New-ScheduledTaskTrigger -AtLogOn
  $principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) `
                 -LogonType Interactive -RunLevel $(if ($Elevated) { 'Highest' } else { 'Limited' })
  $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                 -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
  Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Force | Out-Null
}

Step 'Auto-start'
# Remove tasks from a previous install that this one no longer defines (renamed,
# dropped, or an older naming scheme) so no orphan task fights for a drive letter.
foreach ($t in $stale) {
  Plan "stop + remove stale task $($t.TaskName)"
  if (-not $DryRun) {
    Stop-ScheduledTask -TaskName $t.TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $t.TaskName -Confirm:$false
  }
}

foreach ($m in $mounts) {
  if ($m.Direction -eq 'A') {
    # Direction A is already fast over loopback TCP and needs no VM GUID, so it does
    # not use hvsocket (which would force an elevated, non-visible drive). Non-elevated.
    $exe = Join-Path $InstallDir 'wsldrive.exe'
    $ag  = if ($agentInWsl.ContainsKey($m.Distro) -and $agentInWsl[$m.Distro]) { $agentInWsl[$m.Distro] }
           elseif ($m.WslHome) { "$($m.WslHome)/.local/bin/wsldrived" } else { '' }
    $agentArg = if ($ag) { " --agent $(ConvertTo-WinArg $ag)" } else { '' }
    # Quoted: a distro name or served path with a space would otherwise split.
    $arg = "mount $($m.Drive): --distro $(ConvertTo-WinArg $m.Distro) --wsl-root $(ConvertTo-WinArg $m.WslRoot)$agentArg --port $($m.Port)"
    Register-MountTask $m.TaskName $exe $arg $false
    Ok "$($m.Drive): <= $($m.Distro):$($m.WslRoot) at logon (TCP :$($m.Port))."
  } else {
    # Runs the Linux client inside WSL; it launches the Windows agent over interop.
    $hvArgB = if ($m.UseHv) { ' --hvsocket' } else { '' }
    $winRootFwd = $m.WinRoot -replace '\\','/'
    # sh-quoted: an unquoted `~/my win` became two words, so the task ran
    # `mkdir -p ~/my win` (two directories) and mounted the wrong path.
    $mpSh = ConvertTo-ShPath $m.Mountpoint
    $bcmd = "~/.local/bin/wsldrive mount $mpSh --win-root $(ConvertTo-ShQuoted $winRootFwd) --win-agent $(ConvertTo-ShQuoted $agentWslPath) --port $($m.Port)$hvArgB"
    $arg  = "-d $(ConvertTo-WinArg $m.Distro) -- bash -lc `"mkdir -p $mpSh; $bcmd`""
    # Elevated ONLY when this mount actually uses hvsocket. The elevation buys
    # exactly one thing - Hyper-V admin for the hcsdiag VM-GUID lookup - and it
    # is not free: interop processes started from an elevated wsl.exe are
    # themselves elevated, so the write-capable Windows agent ends up applying
    # write-through mutations under -WinRoot with administrator rights, and
    # anything in the distro holding the token inherits that reach. Over plain
    # TCP there is no GUID to look up, so the task runs Limited and the agent
    # does not.
    Register-MountTask $m.TaskName 'wsl.exe' $arg $m.UseHv
    Ok "$($m.Distro):$($m.Mountpoint) <= $($m.WinRoot) at logon ($(if($m.UseHv){'hvsocket'}else{'TCP'}) :$($m.Port))."
  }
}

# --- WSL restart ---
if ($needShutdown) {
  Step 'Restart WSL'
  $go = if ($NoShutdown) { $false } else { $Yes -or (AskYN 'Restart WSL now so it picks up the socket registration? (closes running WSL sessions)' $true) }
  if ($go) {
    Plan 'wsl --shutdown'
    if (-not $DryRun) { & wsl.exe --shutdown }
    Ok 'WSL restarted.'
  } else {
    Warn 'Skipped. Run "wsl --shutdown" yourself before the fast transport will work.'
  }
}

# --- start the mounts now (so the install leaves working drives, not just tasks) ---
Step 'Start mounts'
foreach ($m in $mounts) {
  Plan "start $($m.TaskName)"
  if (-not $DryRun) { Start-ScheduledTask -TaskName $m.TaskName -ErrorAction SilentlyContinue }
}
if (-not $DryRun) {
  # The mount tasks run at the user's normal level, so their drive letters live in
  # the interactive (non-elevated) session — this elevated installer cannot see
  # them with Test-Path. Report the task state instead, which is what we can know.
  Start-Sleep -Seconds 3
  foreach ($m in $mounts) {
    $st = (Get-ScheduledTask -TaskName $m.TaskName -ErrorAction SilentlyContinue).State
    if ($st -eq 'Running') { Ok "$($m.TaskName) is running." }
    else { Warn "$($m.TaskName) is '$st' (it retries, and starts again at logon)." }
  }
  Say ''
  Say 'Drives appear in your normal (non-elevated) session within a few seconds —'
  Say 'a cold start has to boot WSL first. Check with: Get-PSDrive -PSProvider FileSystem'
}

# ===========================================================================
Step 'Done'
Ok 'wsldrive is installed and will mount automatically at logon.'
if (-not $DryRun) {
  Say 'Mount tasks (start/stop by hand if needed):'
  foreach ($m in $mounts) { Say "    Start-ScheduledTask -TaskName $($m.TaskName)" }
  Say 'Verify the environment:  wsldrive doctor'
  Say "Uninstall:               powershell -File scripts\install.ps1 -Uninstall"
}
