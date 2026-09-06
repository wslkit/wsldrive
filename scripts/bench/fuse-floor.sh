#!/usr/bin/env bash
# fuse-floor.sh <bindir> <tree> <port> [runs]
#
# Measures the mount's own serving floor with the VM boundary taken out of the
# picture: the agent and the mount both run inside WSL and talk over loopback
# TCP (sub-ms round-trip), so what remains is the FUSE upcall cost, the client's
# in-RAM cache and the kernel page cache. That is the part the mount-side
# tuning (cache policy, connection negotiation, multi-threaded loop) changes.
#
# Prints one key=value per line so an A/B run can be diffed directly.
set -uo pipefail
BIN="$1"; TREE="$2"; PORT="$3"; RUNS="${4:-5}"
CLI="$BIN/wsldrive"; AGENT="$BIN/wsldrived"
MP=$(mktemp -d /tmp/fbm.XXXXXX)

# Both ends need the same shared secret; the real launcher sets this itself.
if [ -z "${WSLDRIVE_TOKEN:-}" ]; then
  WSLDRIVE_TOKEN=$(head -c 16 /dev/urandom | od -An -tx1 | tr -dc 'a-f0-9')
  export WSLDRIVE_TOKEN
fi

cleanup() {
  fusermount3 -uz "$MP" 2>/dev/null
  [ -n "${MOUNT_PID:-}" ] && kill "$MOUNT_PID" 2>/dev/null
  [ -n "${AGENT_PID:-}" ] && kill "$AGENT_PID" 2>/dev/null
  wait 2>/dev/null
  rmdir "$MP" 2>/dev/null
  return 0
}
trap cleanup EXIT

"$AGENT" --root "$TREE" --listen "tcp://127.0.0.1:$PORT" --exit-when-idle >/tmp/fb-agent.log 2>&1 &
AGENT_PID=$!
for _ in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q ":$PORT " && break; sleep 0.1; done

"$CLI" mount "$MP" --connect "tcp://127.0.0.1:$PORT" >/tmp/fb-mount.log 2>&1 &
MOUNT_PID=$!
up=no
for _ in $(seq 1 100); do mountpoint -q "$MP" && { up=yes; break; }; sleep 0.1; done
if [ "$up" != yes ]; then echo "MOUNT FAILED"; cat /tmp/fb-mount.log /tmp/fb-agent.log; exit 1; fi

# Wall-clock timing, retried on a non-positive result: WSL's clock is NTP-synced
# and can step backwards between the two reads, which showed up as a negative
# sample that would then sort to the front of a median.
ms() {
  local S E d i
  for i in 1 2 3; do
    S=$(date +%s%N); "$@" >/dev/null 2>&1; E=$(date +%s%N)
    d=$(( (E-S)/1000000 ))
    if [ "$d" -gt 0 ]; then echo "$d"; return 0; fi
  done
  echo "$d"  # give up and report it; a caller can see it is not a real timing
}
walk()    { find "$MP" -type f | wc -l; }
readall() { find "$MP" -type f -print0 | xargs -0 cat; }
statall() { find "$MP" -type f -exec stat -c %s {} +; }
par8()    { find "$MP" -type f -print0 | xargs -0 -P 8 -n 40 cat; }

# Cold = the first pass with nothing warm. Prefetch-on-mount is default-on, so
# let it settle first; otherwise the number races the background prefetcher.
sleep 2
# COLD_PARALLEL=1 measures eight readers against a cache that is still empty, so
# every read has to cross to the agent. That is the one case where a
# multi-threaded FUSE loop could plausibly win, and the warm numbers below
# cannot see it: once the page cache is serving, the daemon is barely involved.
if [ "${COLD_PARALLEL:-0}" = 1 ]; then
  echo "cold_par8_read_ms=$(ms par8)"
  echo "cold_read_ms=$(ms readall)"
  echo "cold_walk_ms=$(ms walk)"
else
  echo "cold_read_ms=$(ms readall)"
  echo "cold_walk_ms=$(ms walk)"
fi

med() {  # median of RUNS timings, one discarded warm-up first
  local fn="$1" i
  local -a t=()
  ms "$fn" >/dev/null
  for ((i=0;i<RUNS;i++)); do t+=("$(ms "$fn")"); done
  printf '%s\n' "${t[@]}" | sort -n | awk -v n="$RUNS" 'NR==int(n/2)+1{print}'
}
echo "warm_walk_ms=$(med walk)"
echo "warm_stat_ms=$(med statall)"
echo "warm_read_ms=$(med readall)"
echo "par8_read_ms=$(med par8)"
