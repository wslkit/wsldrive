# Change notification across the boundary

A file changed by a Windows application does not fire `inotify` inside WSL2. Nothing errors.
`vite`, `webpack --watch`, `nodemon`, `jest --watch`, `tsc --watch`, `air` and `cargo-watch` all
just sit there. It is [microsoft/WSL#4739](https://github.com/microsoft/WSL/issues/4739), and it is
the reason people who keep their source on the Windows side end up polling.

A wsldrive Direction B mount delivers those events. Editing a file from a Windows editor raises a
real `IN_MODIFY` on the mount, a new file raises `IN_CREATE` in its directory, a rename raises a
paired `IN_MOVED_FROM`/`IN_MOVED_TO`, and a deletion raises `IN_DELETE`. Watchers need no
cooperation: no preload, no plugin, no knowledge that wsldrive is involved.

It is on by default. `wsldrive mount --no-inotify` turns it off.

## Why it needs a mechanism at all

The kernel raises fsnotify events from the VFS, at the point an operation is performed.
`vfs_create` calls `fsnotify_create`, `vfs_unlink` calls `fsnotify_unlink`, `notify_change` calls
`fsnotify_change`. A filesystem cannot raise one itself, and FUSE offers nothing that does: the
`fuse_lowlevel_notify_*` calls invalidate dentries and pages, which is cache coherence, not
notification. `fuse_reverse_inval_entry` never touches fsnotify. So a userspace daemon has no way to
hand an inotify watcher an event, and this is why the problem has outlived so many attempts at it.

## What wsldrive does

It asks the kernel to raise the event, by performing on the mount the operation the far side already
performed. A thread inside the mount process replays each change as an ordinary syscall against the
mount's own path:

| far side | wsldrive replays | kernel raises |
|---|---|---|
| file written | `utimensat`, mtime only | `IN_MODIFY` |
| file created | `mknodat` (regular file) | `IN_CREATE` |
| directory created | `mkdirat` | `IN_CREATE` with `IN_ISDIR` |
| file deleted | `unlinkat` | `IN_DELETE` |
| directory deleted | `unlinkat(AT_REMOVEDIR)` | `IN_DELETE` with `IN_ISDIR` |
| renamed | `renameat` | `IN_MOVED_FROM` + `IN_MOVED_TO`, one cookie |

The `utimensat` detail matters. `fsnotify_change()` reports a change to *both* timestamps as
`FS_ATTRIB` and a change to mtime alone as `FS_MODIFY`, so touching only mtime is what turns a
far-side write into `IN_MODIFY` rather than the weaker `IN_ATTRIB`. And letting the kernel perform
the rename is what produces a genuine move cookie, so a watcher sees one move instead of an
unrelated delete and create.

These operations must not cross the boundary a second time — the far side already has this state,
and re-applying it would at best waste a round trip and at worst destroy the file that prompted the
event. So the FUSE handlers recognise the bridge's own requests and answer them locally. That test
is deliberately over-determined, because the cost of getting it wrong is a user's `rm` silently not
removing anything: the request must come from the bridge's own thread, a poke of exactly that kind
must be in flight, and it must name exactly that path. A poke is claimed once and then retired.

The recognition has to cover every handler the poke can reach, which is not always the obvious one.
A `mknodat` of a regular file arrives as `FUSE_MKNOD`, but libfuse offers it to the `create` handler
first and only falls back to `mknod` if that answers `ENOSYS` — so a mount with a `create` handler
never sees the poke at `mknod` at all. Getting that wrong is not a missed event; it forwards the
poke as a genuine creation and replaces the file that prompted it with an empty one.

Which is why an unrecognised request from the bridge's thread is refused rather than forwarded. The
bridge performs no genuine mutations, so a mutation from its thread is one of its own replays and
nothing else. A refused poke costs a notification; a forwarded one costs data.

Two further guards keep the replay inside the mount. The bridge refuses to start unless its root is
a FUSE mount (`statfs` reports `FUSE_SUPER_MAGIC`), and every poke re-checks that its target's
parent is still on that mount's device. Both exist because the pokes are real filesystem calls: aimed
at the wrong tree they would create and delete real files there.

## Two small fictions

Both are visible only to the bridge's own thread; every other caller gets the mirror's real answer.

The invalidation is applied to the metadata mirror before the poke runs, so the mirror is already
telling the truth by the time the kernel looks — which is exactly wrong for making the kernel run
the operation. A creation would find the path already there and never reach `->mknod`; a deletion
would find nothing and never reach `->unlink`. So for the length of one syscall the mount reports a
newly created path as absent, and a newly deleted one as still present. Neither fiction outlives
its call: the create or unlink that follows settles the dentry, and the record is retired the moment
the handler claims it.

## What it costs

One syscall per changed path, answered by the FUSE loop out of the in-RAM mirror. No boundary
crossing and no I/O, so the added latency over a write made locally on the mount is the mount's own
round trip. `scripts/inotify-conformance.sh` measures both and prints them side by side.

The queue is bounded at 65536 pending changes. Past that the oldest are dropped, and `wsldrive mount`
says so on exit. A dropped notification is a late one, not a wrong one: the mirror and the page cache
are already correct, so the file reads right the moment anything looks at it.

## Limits

- **Direction B only.** Direction A does not have this problem: a WinFsp volume raises Windows change
  notifications through its own mechanism.
- **An overflow cannot name paths.** When the far side's watcher loses events it sends a rescan, the
  mirror is rebuilt from a fresh snapshot, and the bridge touches the mount root. A watcher that
  re-walks on any event below its root will catch up; one waiting for a specific path will not be
  told about it. Making an overflow name what changed means diffing the old tree against the new
  snapshot, which is not implemented. In practice the Windows watcher's 1 MiB buffer keeps overflows
  rare outside of bursts on the scale of an `npm install`.
- **`inotify` only.** `fanotify` marks are not delivered, and neither are the `IN_OPEN`, `IN_ACCESS`
  or `IN_CLOSE` classes of event — nothing on the far side reports those, so there is nothing to
  replay.
- **The mount's own watch limits apply.** `max_user_watches` bounds a recursive watcher on the mount
  exactly as it would on any local filesystem; wsldrive neither raises nor consumes that budget.

## Checking it

`scripts/inotify-conformance.sh` mounts a tree, changes it from the serving side, and asserts that
each change raises the right event type on the mount within a timeout. It also checks the thing that
would be worst to get wrong — that the served tree is untouched by the bridge's own operations — and
exercises a recursive watch over a 10,000-file tree. CI runs it on every change.

To watch the event stream by hand without a mount, `wsldrive fetch --connect <endpoint> --watch`
prints each invalidation with its change kind and rename cookie.
