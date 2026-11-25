#!/usr/bin/env bash
# Power / heat per frame-rate mode from the bench save: for each mode, boot to gameplay with ab_run.sh, stay in game
# for IN_GAME_S seconds while tools/android/heat_sampler.sh samples every 10 s, then let the device cool asleep.
#   tools/android/power_campaign.sh <out dir> [modes...]   modes: 60 30 40 s60 (s60 = Smoother 60, 120 Hz panel)
# Env: IN_GAME_S (300), COOL_S (120). Prints per mode: fps (heartbeats in game), CPU / GPU temperature avg+peak,
# GPU MHz / busy, whole-device power (USB input with a full battery).
set -u
cd "$(dirname "$0")/../.."
source tools/android/env.sh
out=$1; shift
modes=${*:-60 30 40 s60}
IN_GAME_S=${IN_GAME_S:-300}; COOL_S=${COOL_S:-120}
mkdir -p "$out"
for m in $modes; do
  case $m in
    60) ex="--es DS2_FPS 60 --es DS2_VBLANK120 0" ;;
    30) ex="--es DS2_FPS 30 --es DS2_VBLANK120 0" ;;
    40) ex="--es DS2_FPS 40 --es DS2_VBLANK120 1" ;;
    s60) ex="--es DS2_FPS 60 --es DS2_VBLANK120 1" ;;
    *) echo "unknown mode $m"; continue ;;
  esac
  PKG=com.ds2recomp.android OUT="$out" bash tools/android/heat_sampler.sh "mode$m" $((IN_GAME_S + 200)) 10 > "$out/mode$m.sampler" 2>&1 &
  sampler=$!
  DS2_RUN_SECS=$((IN_GAME_S + 105)) DS2_SCREENSHOTS="$((IN_GAME_S + 100))" \
    tools/android/ab_run.sh "$out" "mode$m||--es DS2_FRAME_OVERLAY 0 $ex" > "$out/mode$m.run" 2>&1
  wait "$sampler"
  csv=$(ls -t "$out"/mode${m}_*.csv 2>/dev/null | head -1)
  python -I - "$(cygpath -m "$csv")" "$(cygpath -m "$out/mode$m.log")" "$m" <<'EOF'
import csv, re, sys
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r['running'] == '1']
rows = rows[11:]  # skip ~110 s of boot, menus and loading
text = open(sys.argv[2], errors='replace').read()
text = text[max(0, text.rfind('Available Vulkan physical')):]  # the log accumulates launches: this run only
fps = [float(x) for x in re.findall(r'hb fps=([0-9.]+)', text)]
fps = fps[55:] if len(fps) > 60 else fps  # heartbeats every 2 s: skip the first ~110 s (boot, dialogs, loading)
def stat(key):
    v = [float(r[key]) for r in rows if r.get(key) and float(r[key]) > 0]
    return (sum(v) / len(v), max(v)) if v else (0, 0)
cpu, gpu = stat('cpu_c'), stat('gpu_c')
mhz, busy = stat('gpu_mhz'), stat('gpu_busy')
usb = stat('usb_w')
ma = [abs(int(r['batt_ma'])) for r in rows if r.get('batt_ma')]
note = 'USB input, battery full' if ma and sum(ma) / len(ma) < 50 else 'USB input (battery charging: not the device alone)'
print("mode %-3s fps avg %.1f min %.1f | CPU %.1f/%.1f C  GPU %.1f/%.1f C (avg/peak) | GPU %.0f MHz %.0f%% busy | %.2f W (%s), %d samples"
      % (sys.argv[3], sum(fps) / max(len(fps), 1), min(fps) if fps else 0, cpu[0], cpu[1], gpu[0], gpu[1],
         mhz[0], busy[0], usb[0], note, len(rows)))
EOF
  # ab_run/build_android already force-stopped and slept the device if they ran; never send a key while another
  # game runs (a refused mode once put the screen to sleep under another session's LR test).
  other=$(adb shell "pidof com.lrrecomp.android com.ds2recomp.android com.ducvd89.bloodborne com.ds3recomp.android" </dev/null 2>/dev/null | tr -d '')
  [ -z "$other" ] && adb shell input keyevent KEYCODE_SLEEP </dev/null >/dev/null 2>&1
  sleep "$COOL_S"
done
