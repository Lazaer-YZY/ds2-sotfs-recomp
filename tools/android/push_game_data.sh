#!/usr/bin/env bash
# Copy the extracted game (assets/game_root) to the app's game folder on the device. Resumable: files already there
# with the same size are skipped. Does not wake the device.
#   tools/android/push_game_data.sh [local dir] [device subfolder]   (defaults: assets/game_root, game)
# Traps: device paths go to the device shell in single quotes (the disc has a "$SystemUpdate" folder), and every adb
# call inside the loop reads </dev/null, or adb swallows the rest of the file list.
set -u
source "$(dirname "$0")/env.sh"
SRC=${1:-$DS2_ROOT/assets/game_root}
SUB=${2:-game}
PKG=com.ds2recomp.android
DST=/sdcard/Android/data/$PKG/files/$SUB
ADB=(adb ${DS2_ADB_SERIAL:+-s "$DS2_ADB_SERIAL"})
"${ADB[@]}" get-state >/dev/null 2>&1 || { echo "no device"; exit 1; }
"${ADB[@]}" shell mkdir -p "$DST" </dev/null || { echo "cannot create $DST (is the app installed?)"; exit 1; }
dev_size() { "${ADB[@]}" shell "stat -c %s '$1'" </dev/null 2>/dev/null | tr -d '\r'; }
fail=0 pushed=0 have=0
while IFS= read -r -d '' f; do
  rel=${f#"$SRC"/}
  size=$(stat -c %s "$f")
  [ "$(dev_size "$DST/$rel")" = "$size" ] && { have=$((have + 1)); continue; }
  "${ADB[@]}" shell "mkdir -p '$(dirname "$DST/$rel")'" </dev/null >/dev/null 2>&1
  ok=0
  for attempt in 1 2 3; do
    "${ADB[@]}" push "$(cygpath -m "$f")" "$DST/$rel" </dev/null >/dev/null 2>&1   # Windows adb needs a C:/ path
    [ "$(dev_size "$DST/$rel")" = "$size" ] && { ok=1; break; }
    echo "retry $attempt: $rel"
  done
  if [ $ok = 1 ]; then pushed=$((pushed + 1)); else echo "FAILED: $rel"; fail=1; fi
done < <(find "$SRC" -name '$SystemUpdate' -prune -o -type f -print0)   # $SystemUpdate: the console dashboard update
echo "push done: $pushed copied, $have already there, failures: $fail -> $DST"
exit $fail
