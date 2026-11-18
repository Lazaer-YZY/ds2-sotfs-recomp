#!/usr/bin/env bash
# Live pad input for a running development build launched with --ds2_input_live=true (e.g. ab_run.sh with
# DS2_RUN_SECS long enough and that cvar): writes the steps to <app files>/input_live.txt, which the game reads and
# deletes within 100 ms (app/src/ds2_input_script.cpp; step times are seconds from that moment), then optionally
# waits for the steps to finish and takes a screenshot of the game's display.
#   tools/android/pad.sh "<steps>" [<screenshot.png> [wait_s]]
#   e.g. tools/android/pad.sh "0-1.5:lstick_up,2:a" out/device/live/01.png
# Buttons as in ds2_input_script (a b x y lb rb lt rt up down left right start back lstick_* rstick_* lthumb rthumb).
# Only our app reads the file, so nothing reaches another app; refuses unless com.ds2recomp.android is running.
set -u
cd "$(dirname "$0")/../.."
source tools/android/env.sh  # adb on PATH, MSYS_NO_PATHCONV
steps=$1
shot=${2:-}
pid=$(adb shell pidof com.ds2recomp.android </dev/null | tr -d '\r')
[ -n "$pid" ] || { echo "game not running"; exit 1; }
case "$steps" in *"'"*) echo "no quotes in steps"; exit 1 ;; esac
F=/sdcard/Android/data/com.ds2recomp.android/files/input_live.txt
# Written under another name and renamed, so the game never reads half a file.
adb shell "printf '%s' '$steps' > '$F.tmp' && mv '$F.tmp' '$F'" </dev/null
if [ -n "$shot" ]; then
  # Wait for the last step to end (+0.4 s), unless a wait is given.
  w=${3:-$(echo "$steps" | tr ',; ' '\n\n\n' |
    sed -n 's/^\([0-9.]*\)\(-\([0-9.]*\)\)\{0,1\}:.*/\1 \3/p' |
    awk '{e = ($2 != "" ? $2 : $1 + 0.25); if (e > m) m = e} END {printf "%.2f", m + 0.4}')}
  sleep "$w"
  mkdir -p "$(dirname "$shot")"
  adb exec-out screencap -p > "$shot" </dev/null
  echo "$shot"
fi
