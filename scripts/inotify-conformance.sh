#!/usr/bin/env bash
# Change-notification conformance check for a wsldrive mount.
#
# The claim under test is the one from microsoft/WSL#4739: a file changed on the
# *far* side of the boundary fires a real inotify event on this side, so an
# unmodified watch-mode tool reacts to it. Nothing about that can be checked
# from a unit test — it depends on which kernel hook each operation runs, which
# only happens against a live mount.
#
# The far side here is a Linux agent rather than a Windows one, because CI has
# no Windows peer. That is the same code path: the agent watches its tree, the
# client applies the invalidation, and the bridge turns it into a local event.
# What a Windows agent changes is which platform watcher produces the event, and
# that part is covered by the unit tests.
#
#   scripts/inotify-conformance.sh [build-dir]
#
# Exit status is the number of failed checks (0 = all good).

set -u
BUILD=${1:-build/linux-release}
AGENT="$BUILD/src/tools/wsldrived"
CLI="$BUILD/src/tools/wsldrive"
PORT=${PORT:-51998}
# How long a notification may take to arrive. Generous: the agent coalesces for
# up to 25 ms, and a loaded CI runner adds more. The measured figure is reported
# separately at the end.
TIMEOUT=${TIMEOUT:-15}

command -v inotifywait >/dev/null || { echo "inotifywait not found (apt install inotify-tools)"; exit 99; }
for exe in "$AGENT" "$CLI"; do
  [ -x "$exe" ] || { echo "not built: $exe"; exit 99; }
done

WORK=$(mktemp -d)   # the served tree: stands in for the Windows side
MNT=$(mktemp -d)    # the mount: stands in for the Linux side
LOG=$(mktemp -d)
export WSLDRIVE_TOKEN="inotify-conformance-$$"
cleanup() {
  [ -n "${WATCH_PID:-}" ] && kill "$WATCH_PID" 2>/dev/null
  fusermount3 -u "$MNT" 2>/dev/null
  [ -n "${CLI_PID:-}" ] && kill "$CLI_PID" 2>/dev/null
  [ -n "${AGENT_PID:-}" ] && kill "$AGENT_PID" 2>/dev/null
  rm -rf "$WORK" "$MNT" "$LOG" 2>/dev/null
  return 0
}
trap cleanup EXIT

mkdir -p "$WORK/src" "$WORK/keep"
echo "original" > "$WORK/src/app.js"
echo "static" > "$WORK/keep/untouched.txt"

echo "serving $WORK -> $MNT"
"$AGENT" --root "$WORK" --listen "tcp://127.0.0.1:$PORT" >"$LOG/agent.log" 2>&1 &
AGENT_PID=$!
sleep 2
"$CLI" mount "$MNT" --connect "tcp://127.0.0.1:$PORT" >"$LOG/mount.log" 2>&1 &
CLI_PID=$!
for _ in $(seq 1 60); do sleep 1; mountpoint -q "$MNT" && break; done
mountpoint -q "$MNT" || { echo "MOUNT FAILED"; tail -20 "$LOG/mount.log"; exit 98; }
grep -q "inotify watchers" "$LOG/mount.log" || echo "  note: the mount did not report the bridge as enabled"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); printf '  ok    %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$1"; }
# awk rather than bc: bc is not installed on a stock CI runner.
elapsed_ms() { awk -v a="$2" -v b="$1" 'BEGIN { printf "%.0f", (a - b) * 1000 }'; }

# One watcher for the whole run, recursive over the mount, writing every event
# to a file. A fresh inotifywait per check would race with the change it is
# meant to see; this way the log is always already listening.
EVENTS="$LOG/events"
: > "$EVENTS"
inotifywait -m -r -q --format '%e %w%f' -o "$EVENTS" "$MNT" &
WATCH_PID=$!
# inotifywait establishes its watches asynchronously and drops anything that
# happens first, so wait until it says it is ready rather than guessing.
for _ in $(seq 1 100); do
  touch "$MNT/.probe" 2>/dev/null
  grep -q '.probe' "$EVENTS" 2>/dev/null && break
  sleep 0.1
done
rm -f "$MNT/.probe" 2>/dev/null
: > "$EVENTS"

# Waits for a line matching an event-name pattern and a path, and reports how
# long it took. `$2` is an extended regex over the event names as inotifywait
# prints them (e.g. 'CREATE', 'MOVED_(FROM|TO)').
await() {
  local what="$1" events="$2" path="$3" start now
  start=$(date +%s.%N)
  for _ in $(seq 1 $((TIMEOUT * 20))); do
    if grep -Eq "^[A-Z_,]*($events)[A-Z_,]* $MNT/$path\$" "$EVENTS" 2>/dev/null; then
      now=$(date +%s.%N)
      printf '  ok    %-46s %6.0f ms\n' "$what" "$(elapsed_ms "$start" "$now")"
      PASS=$((PASS+1))
      return 0
    fi
    sleep 0.05
  done
  bad "$what"
  echo "        wanted: $events on $path"
  echo "        events seen:"
  sed 's/^/        | /' "$EVENTS" | tail -15
  return 1
}

echo
echo "== each change on the served tree must raise the matching inotify event =="

# Write. The one that matters most: this is a Windows editor saving a file, and
# the event every watch-mode tool is waiting for.
echo "edited by the other side" > "$WORK/src/app.js"
await "write -> MODIFY" "MODIFY" "src/app.js"

# Create.
echo "brand new" > "$WORK/src/added.ts"
await "create -> CREATE" "CREATE" "src/added.ts"

# Create a directory.
mkdir "$WORK/src/components"
await "mkdir -> CREATE,ISDIR" "CREATE" "src/components"

# Rename. Both halves must arrive, which is what tells a watcher this is a move
# and not an unrelated delete plus create.
mv "$WORK/src/added.ts" "$WORK/src/renamed.ts"
await "rename -> MOVED_FROM (old name)" "MOVED_FROM" "src/added.ts"
await "rename -> MOVED_TO (new name)" "MOVED_TO" "src/renamed.ts"

# Delete.
rm "$WORK/src/renamed.ts"
await "delete -> DELETE" "DELETE" "src/renamed.ts"

# Delete a directory.
rmdir "$WORK/src/components"
await "rmdir -> DELETE,ISDIR" "DELETE" "src/components"

echo
echo "== the mount must stay correct while all that is going on =="
if [ "$(cat "$MNT/src/app.js")" = "edited by the other side" ]; then
  ok "the new contents read back through the mount"
else
  bad "the new contents read back through the mount"
  echo "        got: $(cat "$MNT/src/app.js" 2>&1)"
fi
if [ -e "$MNT/src/renamed.ts" ] || [ -e "$MNT/src/added.ts" ]; then
  bad "a deleted file is gone from the mount"
  ls -la "$MNT/src"
else
  ok "a deleted file is gone from the mount"
fi
# The bridge performs real filesystem calls against the mount. If any of them
# were forwarded across the boundary instead of being recognised as its own,
# the served tree would be damaged — this is the check that would catch it.
if [ "$(cat "$WORK/keep/untouched.txt")" = "static" ] && [ "$(cat "$WORK/src/app.js")" = "edited by the other side" ]; then
  ok "the served tree is untouched by the bridge"
else
  bad "the served tree is untouched by the bridge"
  echo "        the bridge's own operations must never reach the agent"
  ls -laR "$WORK"
fi

echo
echo "== recursive watch over a large tree =="
# The scale question from the issue: a Node-sized project is tens of thousands
# of paths, and both the watcher and the bridge have to survive one changing at
# once without overflowing into a rescan that names nothing.
BIG=$WORK/big
mkdir -p "$BIG"
for d in $(seq 1 100); do
  mkdir -p "$BIG/d$d"
  for f in $(seq 1 100); do echo "$f" > "$BIG/d$d/f$f.js"; done
done
# Let the burst settle, then check that the tree arrived and a change deep
# inside it still notifies. A burst this size overruns the watcher and becomes a
# rescan, which is the slow path on purpose (it is rate-limited), so wait for the
# count rather than assuming one interval is enough.
COUNT=0
for _ in $(seq 1 60); do
  COUNT=$(find "$MNT/big" -type f 2>/dev/null | wc -l)
  [ "$COUNT" -eq 10000 ] && break
  sleep 1
done
if [ "$COUNT" -eq 10000 ]; then
  ok "10000 files appeared on the mount"
else
  bad "10000 files appeared on the mount (saw $COUNT)"
fi
: > "$EVENTS"
echo "deep change" > "$BIG/d50/f50.js"
await "write deep in a 10k-file tree -> MODIFY" "MODIFY" "big/d50/f50.js"

echo
echo "== latency, against a write made locally on the mount =="
# The comparison the issue asks for: the same event, once originating on the far
# side and once originating here, so the added cost of crossing the boundary is
# what separates the two numbers.
local_ms() {
  : > "$EVENTS"
  local start now
  start=$(date +%s.%N)
  echo "local" > "$MNT/keep/local.txt"
  for _ in $(seq 1 200); do
    grep -q "$MNT/keep/local.txt" "$EVENTS" 2>/dev/null && break
    sleep 0.01
  done
  now=$(date +%s.%N)
  elapsed_ms "$start" "$now"
}
remote_ms() {
  : > "$EVENTS"
  local start now
  start=$(date +%s.%N)
  echo "remote $RANDOM" > "$WORK/keep/remote.txt"
  for _ in $(seq 1 200); do
    grep -q "$MNT/keep/remote.txt" "$EVENTS" 2>/dev/null && break
    sleep 0.01
  done
  now=$(date +%s.%N)
  elapsed_ms "$start" "$now"
}
printf '  local write  -> event: %6.0f ms\n' "$(local_ms)"
printf '  far-side     -> event: %6.0f ms\n' "$(remote_ms)"
printf '  far-side     -> event: %6.0f ms  (warm)\n' "$(remote_ms)"

echo
echo "passed: $PASS   failed: $FAIL"
if [ "$FAIL" -ne 0 ]; then
  echo "--- agent log (tail) ---"; tail -20 "$LOG/agent.log" 2>/dev/null
  echo "--- mount log (tail) ---"; tail -20 "$LOG/mount.log" 2>/dev/null
fi
exit $FAIL
