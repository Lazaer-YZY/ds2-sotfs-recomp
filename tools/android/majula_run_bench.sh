#!/usr/bin/env bash
# Majula run bench: frame pacing while the character runs, rolls and the camera swings fast (the owner, 2026-11-20:
# "runs around Majula, periodically dodge rolls, moves camera around fast"; his busy play needs about twice the CPU
# of a standing view).
#   tools/android/majula_run_bench.sh <out dir> "<label>|<extra cvars>|<extra launch extras>" ...
# Env: RUN (300: seconds of the pattern), START_MAX_C (50: each variant starts once the CPU is below this),
#      AB_SAVE (out/bench_saves/majula_bonfire), PATTERN (run, or fight below), PRELUDE (pad steps before the pattern,
#      times in seconds from 112 s, e.g. a walk to the enemies; PRELUDE_SECS long), GODMODE_MAXHP (the bench
#      character's maximum HP: keeps its HP up, app/src/ds2_hp_probe.cpp, for fights).
# PATTERN=fight (the owner, 2026-11-20: "lock onto enemies, dodge roll a few times, then 2 swings, then pause for
# stamina, then block, then pause, then repeat ... press those relatively fast"), 8-s cycles: R3 lock-on, rolls left /
# right / back (0.35, 1.15, 1.95 s), step in (2.6-3.3 s), two R1 swings (3.4, 3.9 s), pause, block (LB 5.2-6.6 s),
# pause, R3 release at 7.6 s (the next lock-on takes the nearest enemy, so the fight moves on after a kill).
# Per variant: the cool-down gate, heat_sampler.sh, then ab_run.sh from the bench save with this pad script: the
# title dialogs and Continue (as ab_run's own), then from 112 s RUN seconds of 12-s cycles on the bonfire-arch line:
#   0-3 s run toward the arch (sprint, B held, from 0.6 s), 3.4 s roll forward, 4.3 s R3 (camera behind the
#   character again, so camera drift does not add up), 5-6.5 s full-speed camera swing right then left, 6.6-7.3 s up
#   then down, 7.5-10.5 s run back (sprint from 8.1 s), 10.9 s roll back.
# Forward and back legs are the same, so the character stays on the line (an earlier patrol that turned walked off
# the cliff, DECISIONS.md 2026-11-19). 60 fps at 60 Hz (the owner's setting), no overlay, debug logging on (frame
# breakdowns of frames over 20 ms and their 10-s summaries). Report per variant: what reached the screen
# (sf_present_sampler.sh: SurfaceFlinger's present times; the game's own counters cannot see a finished frame the
# presenter drops or delays), frame_stats.py (the game's frames: average fps, 1 % low, frames over 34 / 45 ms), the 10-s
# frame breakdown means (main thread CPU / waits, job threads CPU / run queue), power and temperatures.
set -u
cd "$(dirname "$0")/../.."
out=$1; shift
mkdir -p "$out"
export MSYS_NO_PATHCONV=1
RUN=${RUN:-300}
START=112
export AB_SAVE=${AB_SAVE-out/bench_saves/majula_bonfire}
PKG=com.ds2recomp.android
PATTERN=${PATTERN:-run}
PRELUDE_SECS=${PRELUDE_SECS:-0}
script=$(awk -v start=$START -v run="$RUN" -v pattern="$PATTERN" -v prelude="${PRELUDE:-}" -v pre="$PRELUDE_SECS" 'BEGIN {
  s = "42:start,46:start"
  for (t = 51; t <= 99; t += 3) s = s "," t ":a"
  # Prelude steps "a-b:button" / "a:button", shifted to start at 112 s.
  n = split(prelude, steps, ",")
  for (i = 1; i <= n; ++i) {
    split(steps[i], kv, ":"); m = split(kv[1], ab, "-")
    s = s "," (m == 2 ? sprintf("%g-%g", start + ab[1], start + ab[2]) : sprintf("%g", start + ab[1])) ":" kv[2]
  }
  if (pattern == "fight") {
    for (c = start + pre; c + 8 <= start + pre + run; c += 8)
      s = s sprintf(",%g:rthumb,%g-%g:lstick_left,%g:b,%g-%g:lstick_right,%g:b,%g-%g:lstick_down,%g:b,%g-%g:lstick_up," \
                    "%g:rb,%g:rb,%g-%g:lb,%g:rthumb",
                    c, c + 0.3, c + 0.6, c + 0.35, c + 1.1, c + 1.4, c + 1.15, c + 1.9, c + 2.2, c + 1.95, c + 2.6,
                    c + 3.3, c + 3.4, c + 3.9, c + 5.2, c + 6.6, c + 7.6)
    print s
    exit
  }
  for (c = start + pre; c + 12 <= start + pre + run; c += 12)
    s = s sprintf(",%g-%g:lstick_up,%g-%g:b,%g-%g:lstick_up,%g:b,%g:rthumb,%g-%g:rstick_right,%g-%g:rstick_left," \
                  "%g-%g:rstick_up,%g-%g:rstick_down,%g-%g:lstick_down,%g-%g:b,%g-%g:lstick_down,%g:b",
                  c, c + 3, c + 0.6, c + 2.8, c + 3.3, c + 3.8, c + 3.4, c + 4.3, c + 5, c + 5.7, c + 5.8, c + 6.5,
                  c + 6.6, c + 6.9, c + 7, c + 7.3, c + 7.5, c + 10.5, c + 8.1, c + 10.3, c + 10.8, c + 11.3, c + 10.9)
  print s }')
cpu_temp() {
  adb shell 'm=0; for z in /sys/class/thermal/thermal_zone*; do case $(cat $z/type) in cpu-*|cpuss-*) t=$(cat $z/temp 2>/dev/null); [ -n "$t" ] && [ "$t" -gt "$m" ] && m=$t;; esac; done; echo $((m / 1000))' </dev/null | tr -d '\r'
}
end=$((START + PRELUDE_SECS + RUN))
god=""
[ -n "${GODMODE_MAXHP:-}" ] && god="--es DS2_GODMODE 1 --es DS2_GODMODE_MAXHP $GODMODE_MAXHP"
for v in "$@"; do
  IFS='|' read -r label cvars extras <<< "$v"
  for i in $(seq 1 90); do t=$(cpu_temp); [ -n "$t" ] && [ "$t" -lt "${START_MAX_C:-50}" ] && break; sleep 10; done
  echo "$label: start CPU $(cpu_temp) C at $(date +%T)"
  PKG=$PKG OUT="$out" bash tools/android/heat_sampler.sh "$label" $((end + 90)) 2 > "$out/$label.sampler" 2>&1 &
  hs=$!
  tools/android/sf_present_sampler.sh "$out/$label.sf" $((end + 120)) > /dev/null 2>&1 &
  sf=$!
  AB_SCRIPT="$script" DS2_RUN_SECS=$((end + 6)) \
    DS2_SCREENSHOTS="$((START + PRELUDE_SECS + 30)) $((START + PRELUDE_SECS + RUN / 2)) $end" \
    tools/android/ab_run.sh "$out" \
    "$label|--frame_breakdown_ms=20 $cvars|--es DS2_FRAME_OVERLAY 0 --es DS2_FPS 60 --es DS2_VBLANK120 0 --es DS2_DIAG 1 $god $extras" \
    > "$out/$label.run" 2>&1
  wait $hs
  kill $sf 2>/dev/null; wait $sf 2>/dev/null
  grep -h "shot:\|launch failed\|missing" "$out/$label.run"
  # The run's last RUN seconds on screen (ab_run stops the game 6 s after the pattern ends).
  python -I tools/android/sf_present_stats.py "$out/$label.sf" -$((RUN + 5)) -7 | sed "s/^/  $label screen: /"
  d=$(ls -dt out/device/logs_* | head -1)
  # The launch's whole log (it rotates at 5 MB): oldest file first.
  cat $(ls "$d"/ds2.*.log 2>/dev/null | sort -t. -k2 -nr) "$d/ds2.log" > "$out/$label.all.log"
  python -I tools/android/frame_stats.py "$out/$label.all.log" $((START + PRELUDE_SECS)) $end |
    sed "s/^/  $label frames: /"
  python -I tools/android/frame_summary.py "$out/$label.all.log" $((START + PRELUDE_SECS)) | sed "s/^/  $label /"
  grep -a "ds2 god mode: player" "$out/$label.all.log" | tail -1 | cut -c1-160 | sed "s/^/  $label /"
  f=$(ls -t "$out/${label}"_*.csv 2>/dev/null | head -1)
  [ -n "$f" ] && python -I tools/android/heat_window.py "$f" $((START + PRELUDE_SECS)) $end |
    sed "s/^/  $label heat: /"
done
