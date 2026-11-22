#!/usr/bin/env bash
# Records both Thor screens while the owner plays (read-only: never starts, stops or touches the game), for showcase
# videos: the game screen and the second-screen HUD, cut together afterwards (the LR port's tools/thor_record.sh).
#   tools/android/thor_record.sh start [MINUTES=30]  -> chained 3-min clips per screen on the device
#   tools/android/thor_record.sh status              -> the clips so far
#   tools/android/thor_record.sh stop [OUT_DIR]      -> stops, pulls the clips to OUT_DIR (default out/device/rec/<time>),
#                                                       deletes them on the device
# Top: 1920x1080, bottom: 1240x1080, H.264 at 24 Mbit/s; the frame rate follows each screen's updates (up to 60).
# Video only (Android's screenrecord has no audio). screenrecord stops each clip at 180 s, so a loop on the device
# chains clips (top_NNN.mp4 / bottom_NNN.mp4); the USB cable may be unplugged in between. Recording costs the game some
# frame time (~10 fps off its 1 % lows in the LR port): never take performance numbers from a recorded session.
set -u
cd "$(dirname "$0")/../.."
source tools/android/env.sh  # adb on PATH, MSYS_NO_PATHCONV
DIR=/sdcard/Movies/ds2_rec
# Display ids: "HWC display 0" is the main (top) screen, the other one the second screen.
ids=$(adb shell dumpsys SurfaceFlinger --display-id </dev/null | tr -d '\r')
TOP=$(echo "$ids" | sed -n 's/^Display \([0-9]*\) (HWC display 0).*/\1/p' | head -1)
BOTTOM=$(echo "$ids" | sed -n 's/^Display \([0-9]*\) (HWC display [1-9][0-9]*).*/\1/p' | head -1)
case "${1:-}" in
  start)
    [ -n "$TOP" ] && [ -n "$BOTTOM" ] || { echo "two displays not found:"; echo "$ids"; exit 1; }
    MIN=${2:-30}; N=$(( (MIN * 60 + 179) / 180 ))
    adb shell "rm -rf $DIR; mkdir -p $DIR" </dev/null
    for side in top bottom; do
      id=$TOP; [ $side = bottom ] && id=$BOTTOM
      adb shell "nohup sh -c 'i=0; while [ \$i -lt $N ] && [ ! -f $DIR/.stop ]; do screenrecord --display-id $id --bit-rate 24000000 --time-limit 180 $DIR/${side}_\$(printf %03d \$i).mp4; i=\$((i+1)); done' >/dev/null 2>&1 &" </dev/null
    done
    echo "recording both screens for up to $MIN min into $DIR ('stop' to end)" ;;
  status)
    adb shell "ls -l $DIR 2>/dev/null; pidof screenrecord" </dev/null | tr -d '\r' ;;
  stop)
    OUT=${2:-out/device/rec/$(date +%Y%m%d_%H%M%S)}
    adb shell "touch $DIR/.stop; pkill -INT screenrecord" </dev/null
    sleep 3
    mkdir -p "$OUT"
    for f in $(adb shell "ls $DIR/*.mp4 2>/dev/null" </dev/null | tr -d '\r'); do
      adb pull "$f" "$OUT/" >/dev/null </dev/null && echo "  $(basename "$f")"
    done
    adb shell "rm -rf $DIR" </dev/null
    echo "clips in $OUT" ;;
  *) sed -n 2,8p "$0"; exit 1 ;;
esac
