<p align="center">
  <img src="assets/logo.svg" alt="wsldrive logo" width="120" height="120">
</p>

# wsldrive

[![CI](https://github.com/zcsizmadia/wsldrive/actions/workflows/ci.yml/badge.svg)](https://github.com/zcsizmadia/wsldrive/actions/workflows/ci.yml)

**Cross the WSL2 filesystem boundary at native speed — in both directions.**

Anyone doing real work in WSL2 hits the same wall: files on the *other* side of the VM boundary are
slow. Opening a WSL source tree from Visual Studio or Explorer crawls over `\\wsl.localhost`; a Linux
build reading `/mnt/c` stalls on every file. It's the single most common WSL2 complaint, and it taxes
every edit-build-test loop that spans the two worlds.

`wsldrive` makes that boundary disappear. It keeps the whole directory tree's metadata in RAM on the
side that's reading, caches file contents, and lets the kernel keep those pages warm behind it — so
cross-boundary access runs **8–19× faster on reads** (more on metadata) than the paths Windows and WSL
ship:

|   | today (OS path) | with wsldrive | speedup |
|---|--:|--:|--:|
| **Windows reading WSL files** (read 3000 files) | 2132 ms (`\\wsl.localhost`) | 259 ms | **~8×** |
| **WSL reading Windows files** (read 3000 files) | 4182 ms (`/mnt/c`, virtiofs) | 225 ms | **~19×** |
| directory walk, either direction | 50–165 ms | 7–11 ms | **~7–15×** |

Both directions are read-write and mount *alongside* the built-in paths — nothing to replace, no kernel
driver of its own (WinFsp on Windows, libfuse3 in WSL). Full numbers in [`bench/RESULTS.md`](bench/RESULTS.md).

- **Direction A — WSL2 ext4 as a Windows drive letter.** Windows-only tools (Visual Studio, GitHub
  Desktop, Explorer/Search, Unity, Office) reach a WSL source tree without paying the `\\wsl.localhost`
  Plan 9 tax.
- **Direction B — a Windows drive mounted inside WSL2.** Linux tools read/write an NTFS tree without the
  `/mnt/c` 9P/virtiofs tax — and a Windows-side edit fires `inotify`, so
  [watch mode works](#watch-mode-works-inotify-across-the-boundary).

WSL2 only. WSL1 has no VM boundary (DrvFs runs in the NT kernel, `\\wsl$` is served in-process), so it
has neither problem and is unsupported by design.

## Install

The installer confirms each choice with you first, then does *everything else itself* — installs the
binaries, checks that [WinFsp](https://winfsp.dev) is present (it tells you where to get it if not;
`install.ps1 -WinFspMsi <path>` chain-installs a downloaded MSI), creates an at-logon task that mounts on
every boot, and removes all of it on uninstall (tasks, running mounts, binaries, the copies staged into
your distro). The only thing you supply is what to mount where.

**Two modes:**

- **Easy (default) — Direction A only.** Mounts a WSL folder as a Windows drive letter over loopback TCP.
  No elevation for the mount, no registry changes, no WSL restart; the drive is visible to your normal
  apps and comes back on every reboot. This is the common case and the biggest WSL2 pain.
- **Advanced — Direction A *and* B, side by side.** Adds a Windows folder mounted *inside* WSL
  (a Linux path, not a drive letter). The installer **measures** the loopback round-trip to your
  distro and picks Direction B's transport from that (see [Transports](#transports)); on current WSL
  that means plain TCP, with no registry changes and no `wsl --shutdown`.

### 1. Download the installer (easiest — no build)

Grab **`wsldrive-setup.exe`** from the [latest release](https://github.com/zcsizmadia/wsldrive/releases/latest)
and run it. The wizard sets up Direction A by default; tick **Advanced** to also add Direction B.

Each release also ships `wsldrive-windows-x64.zip` (everything the script route needs: the Windows and
Linux binaries plus `install.ps1`), the standalone Linux binaries `wsldrive-linux-x64` /
`wsldrived-linux-x64`, and `SHA256SUMS.txt` to verify the downloads (the binaries are unsigned, so
SmartScreen will warn on first run).

### 2. Run the script (no packaging — for CI, locked-down machines, or preference)

Download `wsldrive-windows-x64.zip` from the [latest release](https://github.com/zcsizmadia/wsldrive/releases/latest)
(or clone the repo), then from an **elevated** PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1   # interactive: Direction A (easy)
scripts\install.ps1 -Advanced        # also offer Direction B
scripts\install.ps1 -DryRun          # print the full plan, change nothing
scripts\install.ps1 -Unattended -DriveLetter W -Distro Ubuntu -WslRoot '~' -Yes          # Direction A
scripts\install.ps1 -Unattended -DriveLetter W -WinRoot C:\projects -Mountpoint '~/win' -Yes  # A + B
scripts\install.ps1 -Uninstall       # remove tasks + registration + binaries
```

### Several mounts at once (multiple distros)

Pass a config file and each entry becomes its own mount, with its own port and its own
logon task — so multiple drives and multiple distros coexist:

```powershell
scripts\install.ps1 -Config wsldrive.json
```
```json
{ "mounts": [
    { "drive": "W", "distro": "Ubuntu", "wslRoot": "~" },
    { "drive": "Y", "distro": "Debian", "wslRoot": "~/work" },
    { "winRoot": "C:/projects", "mountpoint": "~/win", "distro": "Ubuntu" }
] }
```

`direction` is inferred (`drive` → A, `winRoot` → B) and `port` is auto-assigned unless you set
it. Colliding drive letters, ports, or task names are rejected up front. See
[`wsldrive.example.json`](wsldrive.example.json). Re-running the installer stops the old mounts,
replaces the binaries, removes tasks the new config no longer defines, and starts the new set.

### 3. Build the installer from source

```powershell
.\scripts\build.ps1                       # build wsldrive.exe / wsldrived.exe first
.\scripts\build-installer.ps1             # compile the wizard -> installer\dist\wsldrive-setup.exe
```

`build-installer.ps1` needs [Inno Setup](https://jrsoftware.org/isdl.php) (pass `-InstallInno` to fetch
it via winget). The GUI wizard is a thin front-end that drives `install.ps1`, so routes 1–3 all do
exactly the same thing. CI dry-runs every `install.ps1` code path and compiles the wizard on each
change; the elevated, real-WSL pass is a short [manual checklist](docs/release-checklist.md) per release. Releases (and their `wsldrive-setup.exe`) are produced automatically by the
`Release` workflow on each `v*` tag. To run wsldrive manually without installing, see [Usage](#usage).

## How it works

Two small user-space binaries, no kernel drivers of wsldrive's own:

- **`wsldrived`** — the *agent*: serves a directory tree (snapshot, file contents, writes) and watches
  it for changes (`ReadDirectoryChangesW`+IOCP on Windows, inotify on Linux), pushing coalesced
  invalidations.
- **`wsldrive`** — the *client*: mounts the served tree as a filesystem (WinFsp on Windows, libfuse3 in
  WSL, one FUSE3 implementation) backed by an in-RAM metadata mirror and a content cache. In WSL it
  also turns each invalidation into a real `inotify` event, so watch-mode tools see far-side changes.

Either binary can host either role, so the same code serves both directions — the direction is just
which side runs the agent and which runs the mount.

**Metadata in RAM.** The whole tree (names, sizes, mtimes, mode→attributes) loads on mount and stays
coherent via pushed invalidations, so `stat`/`readdir`/`open`/negative lookups never cross the boundary.

**Content cache + read-ahead.** Small files (≤ 8 MiB) are cached whole in RAM, keyed by path and
validated against the mirror's (mtime, size); the cache is LRU-evicted to a byte budget (256 MiB by
default) and dropped on invalidation. On a read miss the file's directory is bulk-fetched in one `ReadMany` round-trip, so a
sequential reader pays one request per directory, not per file. On mount the client also **warms the
cache in the background** — it queues every directory whose small files fit the cache budget, so the
first reads a tool makes are already hot and cold-read latency is avoided (disable with `--no-prefetch`).

**Writes** are write-through by default (durability = the source filesystem); `--writeback` coalesces a
file's writes and flushes on `fsync`/close for write-heavy work.

### Transport

The wire protocol is a small framed binary protocol (24-byte little-endian header + payload) over a
stream socket. Two transports:

- **Hyper-V sockets** (`AF_HYPERV` on Windows, `AF_VSOCK` in WSL). WSL2 routes hvsocket
  host→guest, so the **WSL side listens** (`vsock://any:<port>`) and the **Windows side connects**
  (`hv://{<wsl-vm-guid>}:<port>`). Not IP, so no firewall involvement. Needed for Direction B only on
  machines where the loopback relay is slow; see [Enabling hvsocket](#enabling-the-hyper-v-socket-transport).
- **Loopback TCP** — works in NAT and mirrored WSL networking, and needs no setup at all. Always
  used for Windows→WSL (Direction A). Whether it is fast enough for WSL→Windows (Direction B)
  depends on the machine, so wsldrive measures rather than assumes.

#### Which one you get, and why it is measured

hvsocket exists because the WSL localhost relay used to cost **seconds** per round-trip, which made
Direction B unusable over TCP. On current WSL it costs a fraction of a millisecond, and where that
holds plain TCP is the better choice: no `HKLM` registration, no `wsl --shutdown` during install, no
Hyper-V admin at run time — and so no elevated logon task, which means the write-capable Windows
agent no longer runs as Administrator.

This **cannot be decided from your WSL version**. A running WSL2 VM keeps the kernel it booted with,
so a machine whose WSL updated months ago can still be on the old kernel until the VM next restarts —
and two machines reporting the same `wsl --version` can behave differently. So the installer probes:

```powershell
wsldrive probe-transport --distro Ubuntu   # prints the median round-trip in ms
                                           # exit 0 = TCP is fine, 1 = use hvsocket, 2 = could not measure
wsldrive doctor --probe-transport          # the same measurement, in doctor's report
```

Under ~5 ms the installer picks TCP; above it, hvsocket (and then does the registration and the WSL
restart). Pass `-NoHvsocket` to force TCP, or `-Advanced` with hvsocket left on to force that; an
explicit flag always wins over the probe.

### Semantics (ext4 ↔ Windows)

| ext4 | Windows view |
|---|---|
| case-sensitive names | case-preserving; case-colliding siblings flagged, the shadowed one hidden |
| symlinks | readable (`readlink`, following); relative targets resolve on the mount, absolute ones name the serving side; creating links is not supported |
| mode bits | fixed `0644` files / `0755` directories on the mount; the read-only flag is not propagated in either direction, and `chmod` is accepted but not persisted (so `git` and build tools work) — no ACL emulation |
| ns mtime | truncated to 100 ns |
| hard links | reported as independent files (`st_nlink` not preserved) — known limitation |
| names illegal on Windows (`: ? * < > \| "`, control chars, trailing dot/space) | mapped to U+F0xx (WSL/Cygwin convention), round-tripping |
| sockets / fifos / devices | hidden |

A `.wsldriveignore` at the served root (gitignore-style: `node_modules/`, `*.log`, `/build`, …) excludes
paths from the mount and from sync.

### Watch mode works (`inotify` across the boundary)

Edit a file from a Windows editor and `vite`, `nodemon`, `tsc --watch`, `jest --watch`, `air` and
`cargo-watch` reload — on a Direction B mount, with no plugin, no preload and no polling.

They do not, on `/mnt/c` or anywhere else. A change made by a Windows application raises no `inotify`
event inside WSL2, so a Linux watcher never fires. Nothing errors; the tooling just quietly stops
reacting, which is why people lose an afternoon to it before finding
[microsoft/WSL#4739](https://github.com/microsoft/WSL/issues/4739).

wsldrive already carries every far-side change across as an invalidation, so the mount knows what
happened within milliseconds. It turns each one into a real kernel event by replaying the operation
on the mount itself — a write becomes an mtime-only `utimensat` (`IN_MODIFY`), a creation a `mknodat`
(`IN_CREATE`), a rename a `renameat`, so the two halves arrive paired under one cookie rather than as
an unrelated delete and create. The events are the kernel's own, so any watcher sees them.

On by default. `wsldrive mount --no-inotify` turns it off. Mechanism, guarantees and the two things
it does not cover: [`docs/inotify.md`](docs/inotify.md).

### What gets served (and what doesn't)

The default root is your **home directory** (`~`) — that is where the work is, and it keeps the tree
small. You can serve any path, including the whole distro:

```powershell
wsldrive mount W: --distro Ubuntu --wsl-root /          # the entire root filesystem
```

The scan **stays on the root's filesystem**. A directory that is a mount point for another filesystem
is still listed, but appears **empty** rather than being descended into. That is what makes `/`
practical: `/proc`, `/sys`, `/dev` and `/run` show up as empty directories instead of adding ~43k
entries of churn, and `/mnt/c` is not pulled back across the boundary (which would otherwise mirror
your whole Windows drive through WSL and back). On this machine `/` is ~80k entries, which the in-RAM
mirror handles comfortably. Pass `wsldrived --cross-filesystems` to descend into mount points anyway.

**Names the tree cannot represent are dropped, not fatal.** A backslash is a path separator here, but a
legal filename character on Linux — systemd writes `system-systemd\x2dcryptsetup.slice`, so `/usr`
contains such names. Those entries (and anything beneath them) are skipped and reported
(`N entries dropped`), so one odd name never costs you the whole mount.

**Non-goals:** replacing `/mnt/c` (wsldrive mounts alongside it); general-purpose bidirectional sync;
kernel drivers or signing beyond WinFsp's.

## Usage

### Direction A — mount a WSL ext4 tree as a Windows drive

Requires the [WinFsp](https://winfsp.dev) runtime (the Developer/SDK feature is only needed to *build*
wsldrive). From any PowerShell:

```powershell
wsldrive doctor                                            # check WinFsp + WSL
wsldrive mount W: --distro Ubuntu --wsl-root ~/project     # launches the agent in the distro, mounts W:
wsldrive mount W: --distro Ubuntu --wsl-root ~/project --hvsocket   # ... over Hyper-V sockets
```

`--distro` auto-launches `wsldrived` in the distro over `wsl.exe`, mounts, and tears it down on exit
(the agent self-terminates if the Windows side goes away). Ctrl+C unmounts cleanly. To attach to an
agent you started yourself: `wsldrive mount W: --connect tcp://127.0.0.1:7788`.

### Direction B — mount a Windows drive inside WSL

One command, run in WSL (launches the Windows agent via interop and mounts over hvsocket):

```bash
wsldrive mount /tmp/win --win-root 'C:/project' \
  --win-agent /mnt/c/path/to/wsldrived.exe --hvsocket
```

Or attach to an agent started on Windows yourself:

```bash
# on Windows:  wsldrived.exe --root C:\project --listen tcp://127.0.0.1:7788
# in WSL:      wsldrive mount /tmp/win --connect tcp://127.0.0.1:7788
# Keep the agent on loopback: WSL2 forwards localhost, so binding 0.0.0.0 buys
# nothing and would expose a read-write file server to the whole network.
```

### Enabling the Hyper-V socket transport

WSL2 only routes registered hvsocket services, so once per machine:

1. `powershell -ExecutionPolicy Bypass -File scripts\register-hvsocket.ps1` **(elevated)** — registers
   the vsock service GUIDs (ports 5700–5709).
2. `wsl --shutdown` so the VM picks them up.
3. The WSL VM GUID is discovered automatically via `hcsdiag` (needs Hyper-V admin) or passed with
   `--vm-guid <guid>`.

## Benchmarks

Real-world cross-boundary numbers from `scripts/bench-real.ps1` on this dev machine (3000 small files
across 60 dirs; median warm run, ms; "native" = the same workload run locally on the owning OS — the
floor). Full methodology and notes in [`bench/RESULTS.md`](bench/RESULTS.md).

**Direction A — Windows reading a WSL2 ext4 tree:**

| scenario | walk (enumerate) | read (all bytes) |
|---|--:|--:|
| native (in WSL, reference) | 4 ms | 39 ms |
| `\\wsl.localhost` (Plan 9) | 104 ms | 2856 ms |
| **wsldrive drive** | **39 ms** | **cold 574 / warm 366 ms** |

**Direction B — WSL reading a Windows NTFS tree:**

| scenario | walk (enumerate) | read (all bytes) |
|---|--:|--:|
| native (on Windows, reference) | 10 ms | 118 ms |
| `/mnt/c` (9P) | 119 ms | 5853 ms |
| `/mnt/c` (virtiofs=true) | 406 ms | 7686 ms |
| **wsldrive over hvsocket** | **12 ms** | **cold 621 / warm 383 ms** |

So ~2.7× walk / ~5–8× read vs `\\wsl.localhost` (Direction A), and ~10× vs both `/mnt/c` transports
(Direction B) — with near-native metadata in both directions. Core micro-benchmarks (`core_bench`,
RelWithDebInfo):

| operation | time |
|---|---|
| path lookup, 3 components | ~150 ns |
| readdir, 100 entries | ~150 ns |
| (parent,name)→node probe, 1M entries, hit / miss | 15 ns / 14 ns (`std::unordered_map`: 44 ns) |
| snapshot encode / decode, 100k entries | 2.5 ms / 3.2 ms (36 bytes/entry) |
| coalesce 30k watcher events (10k-file checkout) | ~7 ms |
| BLAKE3 | 4.4 GiB/s |

Run them with `.\scripts\build.ps1 -Bench`, or the end-to-end suite with `.\scripts\bench-real.ps1`.

## Build

Modern C++23, CMake ≥ 3.28 + presets; dependencies (GoogleTest, Google Benchmark, BLAKE3) are fetched by
CMake with pinned hashes. Builds warning-clean under `/W4` (MSVC) and `-Wall -Wextra -Wpedantic
-Wconversion …` (GCC), warnings-as-errors including the linker.

Windows (Visual Studio 2022):

```powershell
.\scripts\build.ps1            # configure + build + unit tests (RelWithDebInfo)
.\scripts\build.ps1 -Bench     # ... and run the micro-benchmarks
```

WSL2 (Ubuntu 22.04+ / Debian 12+):

```bash
sudo ./scripts/wsl-setup.sh    # build-essential, cmake, ninja, libfuse3-dev
cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release
```

The WinFsp mount builds only when WinFsp is detected (Windows); the libfuse3 mount only when libfuse3 is
detected (Linux) — so CI and minimal builds are unaffected.

## Layout

`src/core` — platform-independent library (metadata tree, string pool, framed protocol, coalescer,
auth token, ignore rules, name escaping, path utils), all unit-tested. `src/net` — sockets
(TCP/vsock/hvsocket) and the framed channel. `src/platform` — watchers and process launchers per OS.
`src/agent` — the scanner, `RootServer`, and the `RemoteRoot` client. `src/mount` — the FUSE3 mount
and the `inotify` bridge.
`src/tools` — the `wsldrive` and `wsldrived` binaries. `tests`, `bench`, `scripts` as named.

## Security

`wsldrived` serves reads *and* write-through mutations (write, create, mkdir, unlink, rmdir, rename,
truncate), so a peer that gets past the handshake can modify the served tree with your permissions.

**Peers authenticate with a per-mount shared secret.** The launcher generates a 128-bit token from the
platform CSPRNG and passes it to both ends through the *environment* (forwarded across the WSL boundary
with `WSLENV`, so it never appears in a command line, which any local process can read). The agent
compares it in constant time during the `Hello` handshake and drops the session on mismatch. This is
automatic for `wsldrive mount` — you never see the token.

Running the agent by hand requires the same: set `WSLDRIVE_TOKEN` in its environment and in the
client's. `wsldrived` **refuses to start** without one unless you pass `--insecure-no-auth`, which
accepts any local peer — only do that on a machine where you trust every process.

Paths from a peer are confined to the served root: `..`, absolute and drive-letter paths are rejected
and the resolved path is re-checked against the root. Note that a **symlink inside the tree is
followed**, so a link pointing outside the root resolves to its target — don't serve a tree containing
links you would not expose.

Release binaries are **unsigned** (Windows SmartScreen will warn); each release publishes
`SHA256SUMS.txt` so downloads can be verified.

## Changelog

Release notes and known limitations: [CHANGELOG.md](CHANGELOG.md).

## License

[MIT](LICENSE). WinFsp (a runtime dependency for Direction A) is licensed separately under GPLv3 or a
commercial license and is **not** bundled — install it from winfsp.dev (the installer checks for it and
points you there).
