# Real-world benchmark results

Measured with `scripts/bench-real.ps1` on this dev machine: WSL2 (Ubuntu 24.04,
kernel 6.6) on Windows 11, `virtiofs` not enabled (default 9P for `/mnt/c`).
Trees are 3000 small files across 60 directories. Median of warm runs, in
milliseconds (lower is better). "native" is the same workload run locally on the
owning OS — the theoretical floor, shown for reference.

## Direction A — Windows accessing a WSL2 ext4 tree

| scenario                    | walk (enumerate) | read (all bytes) |
|-----------------------------|-----------------:|-----------------:|
| native (in WSL, reference)  |            4 ms  |          39 ms   |
| `\\wsl.localhost` (Plan 9)  |          104 ms  |        2856 ms   |
| **wsldrive drive**          |       **39 ms**  |  **cold 574 / warm 366 ms** |

**wsldrive is ~2.7× faster on metadata (walk) and ~5–8× faster on reads than the
`\\wsl.localhost` Plan 9 path** — the everyday path for Windows tools (Visual
Studio, Explorer, Git for Windows) reaching WSL files. Reads are served with
directory read-ahead (one bulk request per directory) and, once warm, from the
in-RAM cache.

## Direction B — WSL accessing a Windows NTFS tree

| scenario                     | walk (enumerate) | read (all bytes) |
|------------------------------|-----------------:|-----------------:|
| native (on Windows, ref)     |          10 ms   |         118 ms   |
| `/mnt/c` (9P)                |         119 ms   |        5853 ms   |
| `/mnt/c` (**virtiofs=true**) |         406 ms   |        7686 ms   |
| wsldrive over TCP relay      |          12 ms   |  minutes (transport-bound) |
| **wsldrive over hvsocket**   |       **12 ms**  |  **cold 621 / warm 383 ms** |

With the **Hyper-V socket transport** (ping **0.114 ms**, vs seconds/round-trip
over the localhost relay), read-ahead and the cache make wsldrive **~10× faster
than both 9P and virtiofs** on reads and near-native on metadata — well past the
2× gate. This is the configuration to use for Direction B.

`/mnt/c` reads are ~50-65× slower than native — the well-known WSL pain.
Notably, on this machine **`virtiofs=true` did not beat 9P** for these
workloads (walk 406 ms, read 7686 ms; five warm runs stayed 5.9-9.8 s), and it
shows no persistent read cache. So the Direction B bar (virtiofs) is still
multi-second — leaving clear room for wsldrive to win once cold reads are
addressed (metadata is already RAM-served; warm reads are cache hits). The
wsldrive FUSE mount is functional (mounts, reads, writes; verified end-to-end)
and serves **all metadata from the client's in-RAM mirror**, so `walk`/`stat`
workloads avoid the per-op boundary cost the same way Direction A does.

**The Direction B bottleneck was the transport, and Hyper-V sockets fix it.**
Reads use directory read-ahead (one bulk `ReadMany` request per directory), but
over the localhost-forwarding relay each guest→host round-trip cost seconds
(this machine runs `networkingMode=mirrored`), so bulk reads were still slow.
Switching the transport to `AF_VSOCK`/`AF_HYPERV` drops the round-trip to
~0.1 ms and makes Direction B fast (table above).

### Enabling the Hyper-V socket transport

WSL2 routes hvsocket **host→guest**, so wsldrive uses: the **WSL side listens**
on `vsock://any:<port>` and the **Windows side connects** to
`hv://{<wsl-vm-guid>}:<port>` — for both directions (in Direction B the Windows
agent dials the WSL client; in Direction A the Windows client dials the WSL
agent). Setup:

1. Register the vsock service GUIDs once, elevated (see
   `scripts/register-hvsocket.ps1`) — WSL2 only routes registered services.
2. Restart WSL (`wsl --shutdown`) so the VM picks up the registered services.

The `--hvsocket` flag then wires everything: the VM GUID is auto-discovered via
`hcsdiag` (or passed with `--vm-guid`), a registered vsock port is chosen, the
agent is auto-launched, and the client connects over `AF_HYPERV`:

```powershell
# Direction A (on Windows): mount a WSL tree over hvsocket
wsldrive mount W: --distro Ubuntu --wsl-root ~/project --hvsocket
```
```bash
# Direction B (in WSL): mount a Windows tree over hvsocket
wsldrive mount /tmp/win --win-root 'C:/project' --win-agent /mnt/c/.../wsldrived.exe --hvsocket
```

Direction A is also fast over plain loopback TCP and does not require hvsocket.

## What wsldrive speeds up, and what it does not

The cost of the OS cross-boundary paths is **per operation**, not per byte, so
that is where the gains are and are not:

- **Big wins — metadata and many-small-file work.** `stat`, `readdir`, negative
  lookups and opens are answered from the client's in-RAM mirror without
  crossing the boundary at all, so enumerating or scanning a tree is many times
  faster. Anything that touches thousands of files (indexers, build systems,
  search, status-style commands) is in this category.
- **Little or no win — bulk sequential I/O.** Streaming a handful of large files
  is something the built-in paths already do at reasonable speed; there is no
  per-file overhead left for wsldrive to remove, and measurements there come out
  roughly level.
- **Not the point — one-off bulk transfers.** If the goal is simply to get a
  copy of a tree onto the other side once, an archive stream (one sequential
  transfer, then unpack natively) beats any filesystem doing the work file by
  file. wsldrive exists for trees you keep *working with* across the boundary,
  where copying is not an option because both sides must see the same live
  files.

Writes are write-through, so they cross the boundary either way; wsldrive
reduces the surrounding metadata cost rather than the write itself.

## Cold reads — prefetch-on-mount

Cold reads (the first touch of a file, before it is cached) used to pay the full
boundary fetch. The client now **warms the cache in the background on mount** —
it queues every directory whose small files fit the cache budget, so a tool's
first reads are already hot. A/B on Direction A (loopback TCP, 3000 files,
**first full read pass right after a fresh mount**, median of 5):

| mount | cold first-read |
|-------|----------------:|
| `--no-prefetch` | 585 ms |
| **prefetch-on-mount** (default) | **423 ms** |

That is ~28 % faster, landing near the ~366 ms warm floor — i.e. warm-up removes
most of the cold penalty that sits on top of the fixed per-file mount-serving
cost. It is transport-agnostic (it warms the client cache regardless of
transport) and on by default; disable with `--no-prefetch`. New `Stats`
counters (`read_miss_fetch_ns`, `prefetch_files`, `prefetch_bytes`) expose the
boundary-fetch time and prefetch coverage. Huge trees (small-file bytes beyond
the cache cap) skip warm-up and fall back to lazy per-directory read-ahead;
pipelined `ReadMany` and a persistent on-disk cache are the follow-ups for that
case.

## Performance work landed

- **Prefetch-on-mount**: after the snapshot the client bulk-fetches the tree's
  small files in the background (bounded by the cache budget), so cold reads are
  avoided for typical trees (see above). `--no-prefetch` opts out.

- **Client-side read cache** (`RemoteRoot`): small files cached whole in RAM,
  validated by (mtime, size), LRU-evicted, invalidated on change. Repeat reads
  are served locally with no round-trip.
- **All metadata in RAM** on the accessing side (already in the design): `stat`,
  `readdir`, negative lookups never cross the boundary — the source of the
  Direction A walk win and the Direction B metadata win.
- **Directory read-ahead**: on a read miss, the file's directory is bulk-fetched
  in one `ReadMany` request (with an async prefetcher for large directories), so
  a sequential reader pays one round-trip per directory rather than per file.

## Mount-side serving floor (kernel page cache + FUSE connection tuning)

The numbers above measure the whole path, so the boundary dominates them and the
mount's own cost is hard to see. `scripts/bench/fuse-floor.sh` isolates it: the
agent and the mount both run **inside WSL** and talk over loopback TCP
(sub-millisecond), so what is left is the FUSE upcall cost, the client's RAM
cache and the kernel page cache. 3000 files, medians of 5 warm runs, three
interleaved A/B pairs.

| workload (Direction B mount, libfuse) | before | after | speedup |
|---------------------------------------|-------:|------:|--------:|
| warm read (whole tree)                | 400 ms | 222 ms | **1.8×** |
| warm read, 8 readers in parallel      | 149 ms |  78 ms | **1.9×** |
| cold read (first pass)                | ~400 ms | ~380 ms | 1.05× |
| walk / stat                           | 10-15 ms | 10-15 ms | unchanged |

What changed: the mount had `kernel_cache = 0`, so the kernel dropped a file's
pages on every open and re-asked the daemon for bytes the client already held in
RAM. It now uses `auto_cache` (pages kept, revalidated against `(mtime, size)`
on the next open — and `getattr` is answered from the in-RAM mirror, so the
check costs no round-trip), plus deliberate `max_write` / `max_readahead` /
`max_background` and splice negotiation instead of defaults. Metadata is
untouched because it was already served from RAM, and cold reads are bounded by
the boundary fetch rather than by the mount.

**Direction A (WinFsp) is unchanged** by this — 274 ms before, 274 ms after,
median of six runs each. Its baseline was already at the level Direction B has
now reached, which is what you would expect if the Windows cache manager was
already retaining the pages that libfuse was throwing away.

That one needed care to establish, and the trap is worth writing down. Run the
pairs one way round (before, then after) and Direction A looks **12% faster**
after the change; run them the other way (after, then before) and it looks
faster *before*. Whichever variant runs **second in a pair** measures ~10%
quicker, because the WSL VM, the agent's page cache and the Windows cache
manager all keep warming across runs. Only pooling both orderings cancels it,
and then the two medians land on the same number. A one-directional A/B here
would have reported a win that does not exist — so alternate the order for
anything measured through a real mount, not just interleaved pairs.

### Measured and rejected: `fuse_loop_mt`

The single-threaded `fuse_loop()` looked like an obvious bottleneck — it retires
one request at a time — so the multi-threaded loop was tried and measured:

| config | warm read | 8 parallel |
|---|--:|--:|
| baseline | 443 ms | 150 ms |
| `fuse_loop_mt` only | 465 ms | 222 ms |
| `auto_cache` + tuning + `fuse_loop_mt` | 289 ms | 117 ms |
| **`auto_cache` + tuning, single-threaded** | **210 ms** | **78 ms** |

It lost in every configuration, so it was not adopted. Behind the page cache the
daemon is barely on the request path, and the requests that do reach it spend
their time under `RemoteRoot`'s single cache mutex, which more threads only
contend for. The case this comparison cannot see is parallel *cold* reads across
a real boundary; that is worth re-measuring after the cache-lock contention is
addressed, and the loop carries a comment saying so.

### Staleness window

Letting the kernel keep pages puts a bound on how late a far-side change can be
noticed. The mount punches the affected pages out as invalidations arrive; with
a file held open across a far-side rewrite, kernel 6.18 served:

| | stale for |
|---|--:|
| with the invalidation punch | 5 ms |
| without it (attribute timeout only) | 981 ms |

### Reproducing these

```bash
# in WSL, from the repo root
bash scripts/bench/gen-tree.sh ~/fuse-floor-bench 3000
bash scripts/bench/fuse-floor.sh "$PWD/build/linux-release/src/tools" ~/fuse-floor-bench 51901 5
```

For an A/B, build the comparison commit into a separate tree
(`git worktree add /tmp/base <ref>`) and run the same script against both binary
directories in **alternating order**, for the reason above. Pick ports outside
Windows' reserved TCP ranges — they are dynamic, so read them off
`netsh interface ipv4 show excludedportrange protocol=tcp` rather than assuming
a fixed window; a reserved port shows up as `MOUNT FAILED`, because WSL2 honours
the reservation too.

## Re-measured 2026-09-05 (WSL 2.7.8.0, kernel 6.18.33.1)

Everything above was measured on WSL kernel 6.6 before the mount-side caching
work landed. Both sides of every comparison have moved since, so the whole table
was taken again on an idle machine: 3000 files, warm medians of five runs,
alternating the order of the two variants (see the ordering bias noted above).

| | OS path | wsldrive | speedup |
|---|--:|--:|--:|
| **Direction A** read (Windows → WSL ext4) | 2132 ms (`\wsl.localhost`) | 259 ms | **8.2×** |
| Direction A walk | 50 ms | 7.5 ms | 6.7× |
| **Direction B** read (WSL → Windows NTFS) | 4182 ms (`/mnt/c`, virtiofs) | 225 ms | **18.6×** |
| Direction B walk | 165 ms | 11 ms | 15× |

The ratios the README claims still hold and are now conservative on Direction B.
The absolute numbers all improved, from both directions at once:

- **The OS paths got faster.** `\wsl.localhost` went 2856 → 2132 ms and
  `/mnt/c` 5853 ms (9P) / 7686 ms (virtiofs) → 4182 ms (virtiofs), with no
  change on our side. Newer WSL, not newer wsldrive.
- **wsldrive got faster**, from the page-cache and connection work: Direction A
  366 → 259 ms, Direction B 383 → 225 ms.

### Direction B no longer needs the Hyper-V socket transport here

This contradicts the finding recorded above, so it is worth stating plainly.
Direction B used to be unusable over loopback TCP on this machine — each
round-trip through WSL's localhost forwarding cost seconds, which is what made
the Hyper-V socket transport necessary. On WSL 2.7.8.0, with the same
`networkingMode=mirrored` configuration, a TCP round-trip is **0.224 ms**, and
the Direction B figures above were measured over plain TCP with no hvsocket
registration at all.

That is one machine and one WSL version, so the hvsocket path stays and stays
recommended; but the claim that Direction B *requires* it no longer holds here,
and it is worth re-checking before treating hvsocket setup as mandatory.

#### It was not a recent update, and that matters

The obvious reading — "a WSL update fixed it" — is wrong, and the real story is
more useful. Nobody updated anything here:

| | |
|---|---|
| kernel 6.18.33.1 built | 5 Jun 2026 |
| WSL package 2.7.8.0 staged on this machine | 26 Jun 2026 |
| slow-relay numbers above recorded, on **kernel 6.6** | 27-29 Aug 2026 |
| WSL VM restarted (now running 6.18) | 1 Sep 2026 |
| TCP round-trip measured at 0.224 ms | 5 Sep 2026 |

A running WSL2 VM keeps the kernel it booted with; updating the package on disk
does not restart it. So the newer kernel sat installed and unused for two months
while the VM carried on with 6.6, and the improvement only appeared when the VM
next restarted.

The practical consequence: **you cannot tell which behaviour you will get from
the installed WSL version alone.** It depends on whether the VM has restarted
since the update. That is a strong argument for probing the transport at run
time rather than gating on a version number, and it means other machines may be
sitting on the same latent improvement without knowing.

### The mutual handshake costs nothing measurable

The handshake gained a third leg when both ends started authenticating each
other. It is a per-connection cost, so no read or walk benchmark would show it.
Timing connect + handshake + snapshot, 25 samples per round, three rounds
alternating:

| | median |
|---|--:|
| before (two legs) | 15 / 15 / 15 ms |
| after (three legs, mutual) | 16 / 15 / 16 ms |

One extra loopback round-trip and a BLAKE3 hash against ~15 ms of process start
and snapshot transfer. A first pass over only 15 samples suggested 16 → 37 ms;
that was noise (min 15, max 134) and did not survive more samples.
