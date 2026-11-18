#!/usr/bin/env bash
# Power / heat A/B of launch variants from the bench save (power_campaign.sh's method for arbitrary variants): for
# each "label|cvars|extras", boot to gameplay with ab_run.sh, stay in game for IN_GAME_S seconds while
# heat_sampler.sh samples every 10 s, then let the device cool asleep.
#   tools/android/power_ab.sh <out dir> "<label>|<cvars>|<am extras>" ...
#   e.g. "noprime||--es DS2_MAIN_PRIME 0"   (frame rate: --es DS2_FPS 60 --es DS2_VBLANK120 0 is added unless given)
# Env: IN_GAME_S (300), COOL_S (150). Prints per variant: fps, 1% low (frame_stats.py), CPU / GPU temperature
# avg+peak, GPU MHz / busy, CPU cluster clocks, whole-device power (USB input with a full battery).
set -u
cd "$(dirname "$0")/../.."
source tools/android/env.sh
out=$1; shift
IN_GAME_S=${IN_GAME_S:-300}; COOL_S=${COOL_S:-150}
mkdir -p "$out"
for v in "$@"; do
  label=${v%%|*}; rest=${v#*|}; cvars=${rest%%|*}; extras=""
  case "$rest" in *"|"*) extras=${rest#*|} ;; esac
  case "$extras" in *DS2_FPS*) ;; *) extras="--es DS2_FPS 60 --es DS2_VBLANK120 0 $extras" ;; esac
  PKG=com.ds2recomp.android OUT="$out" bash tools/android/heat_sampler.sh "$label" $((IN_GAME_S + 200)) 10 > "$out/$label.sampler" 2>&1 &
  sampler=$!
  DS2_RUN_SECS=$((IN_GAME_S + 105)) DS2_SCREENSHOTS="$((IN_GAME_S + 100))" \
    tools/android/ab_run.sh "$out" "$label|$cvars|--es DS2_FRAME_OVERLAY 0 $extras" > "$out/$label.run" 2>&1
  wait "$sampler"
  csv=$(ls -t "$out"/${label}_*.csv 2>/dev/null | head -1)
  if [ -f "$out/$label.log" ] && [ -n "$csv" ]; then
    python -I - "$(cygpath -m "$csv")" "$(cygpath -m "$out/$label.log")" "$label" <<'EOF'
import csv, re, sys
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r['running'] == '1'][11:]
text = open(sys.argv[2], errors='replace').read()
text = text[max(0, text.rfind('Available Vulkan physical')):]
fps = [float(x) for x in re.findall(r'hb fps=([0-9.]+)', text)]
fps = fps[55:] if len(fps) > 60 else fps
def stat(key, scale=1.0):
    v = [float(r[key]) * scale for r in rows if r.get(key) and float(r[key]) > 0]
    return (sum(v) / len(v), max(v)) if v else (0, 0)
cpu, gpu = stat('cpu_c'), stat('gpu_c')
mhz, busy, usb = stat('gpu_mhz'), stat('gpu_busy'), stat('usb_w')
c0, c3, c7 = stat('cpu0_khz', 1e-6), stat('cpu3_khz', 1e-6), stat('cpu7_khz', 1e-6)
print("%-12s fps %.1f (min %.1f) | CPU %.1f/%.1f C  GPU %.1f/%.1f C | GPU %.0f MHz %.0f%% | clocks little %.2f middle %.2f prime %.2f GHz | %.2f W, %d samples"
      % (sys.argv[3], sum(fps) / max(len(fps), 1), min(fps) if fps else 0, cpu[0], cpu[1], gpu[0], gpu[1],
         mhz[0], busy[0], c0[0], c3[0], c7[0], usb[0], len(rows)))
EOF
    python -I tools/android/frame_stats.py "$out/$label.log" 110 99999 2>/dev/null | sed "s/^/  $label: /"
  else
    echo "$label: no run (see $out/$label.run)"
  fi
  other=$(adb shell "pidof com.lrrecomp.android com.ds2recomp.android" </dev/null 2>/dev/null | tr -d '\r')
  [ -z "$other" ] && adb shell input keyevent KEYCODE_SLEEP </dev/null >/dev/null 2>&1
  sleep "$COOL_S"
done
