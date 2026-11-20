#!/usr/bin/env bash
# What reaches the screen: SurfaceFlinger's present times of the game's layer while it runs (read-only).
#   tools/android/sf_present_sampler.sh <out file> [max_seconds=900] [start_delay_s=0]
# The game's own frame counters (heartbeats, the performance overlay) count VdSwap calls; the presenter may still drop
# or delay a finished frame (e.g. when it paints from the UI thread while an ImGui dialog such as the overlay exists).
# `dumpsys SurfaceFlinger --latency <layer>` keeps the last 128 frames (desired present, actual present, frame ready,
# ns); this reads it every 1.5 s and appends the new rows. Summary: tools/android/sf_present_stats.py <out file>.
set -u
export MSYS_NO_PATHCONV=1
out=$1
max=${2:-900}
delay=${3:-0}
PKG=com.ds2recomp.android
sleep "$delay"
: > "$out"
start=$(date +%s)
layer=""
last=0
while [ $(( $(date +%s) - start )) -lt "$max" ]; do
  if [ -z "$layer" ]; then
    layer=$(adb shell "dumpsys SurfaceFlinger --list" </dev/null | tr -d '\r' | grep -m1 "^SurfaceView\[$PKG/.*(BLAST)#")
    [ -z "$layer" ] && { sleep 2; continue; }
  fi
  rows=$(adb shell "dumpsys SurfaceFlinger --latency '$layer'" </dev/null | tr -d '\r' | awk 'NF == 3 && $2 > 0 && $2 < 9e18')
  if [ -z "$rows" ]; then
    adb shell pidof $PKG </dev/null >/dev/null || [ "$last" = 0 ] || break  # the game is gone
    layer=""
    sleep 2
    continue
  fi
  new=$(awk -v last="$last" '$2 > last' <<< "$rows")
  if [ -n "$new" ]; then
    printf '%s\n' "$new" >> "$out"
    last=$(tail -1 <<< "$new" | awk '{print $2}')
  fi
  sleep 1.5
done
