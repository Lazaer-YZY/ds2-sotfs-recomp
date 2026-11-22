#!/usr/bin/env bash
# A/B runs from the newest save on the device: each variant boots, goes through the title dialogs, Continue and the
# first save in the list, then takes screenshots; the screenshots, the run output and the game log are kept per variant.
#   tools/android/ab_run.sh <out dir> "<label>|<extra cvars>" ["<label>|<extra cvars>" ...]
# Env: DS2_RUN_SECS (145), DS2_SCREENSHOTS ("115 130 145"), AB_SCRIPT (the pad script, see below),
#      AB_SAVE (out/bench_saves/things_betwixt_spawn, or a snapshot out/save_snapshots/<stamp>): <xuid>/465307E4/...
#      folders pushed before every variant so runs start from the same spot ("" = the owner's own save, still backed up
#      and restored). The owner's save on the
#      device is backed up first (out/save_backups/<stamp>-bench, checked by hash; the run stops if that fails) and
#      exactly that save is pushed back at the end, also checked. Bench launches never take save snapshots (DS2_SAVE_SNAPSHOTS=0).
# Every screenshot is checked after its variant (shot_check.py; verdicts printed as "<label> shot: PASS/FAIL ...");
# look at the screenshots too.
# The pad script presses START twice at the title and A every 3 s from 51 s to 99 s (the boot dialogs - "not exited
# properly", "no Ethernet cable", "offline mode" - then Continue and the save list; once in the game A is "interact",
# harmless at the bonfire/spawn points used for tests). Device rules as in build_android.sh (it does each launch).
set -euo pipefail
cd "$(dirname "$0")/../.."
source tools/android/env.sh  # adb on PATH, MSYS_NO_PATHCONV
out=$1
shift
mkdir -p "$out"
if [ -z "${AB_SCRIPT:-}" ]; then
  AB_SCRIPT="42:start,46:start"
  for t in $(seq 51 3 99); do AB_SCRIPT+=",$t:a"; done
fi
AB_SAVE=${AB_SAVE-out/bench_saves/things_betwixt_spawn}
DEV_USER=/sdcard/Android/data/com.ds2recomp.android/files/user
dev_hashes() {  # sorted "md5  <xuid>/465307E4/..." of the device's save files
  MSYS_NO_PATHCONV=1 adb shell "cd '$DEV_USER' 2>/dev/null && for d in */465307E4; do [ -d \"\$d/00000001\" ] && find \"\$d\" -type f; done | while read -r f; do md5sum \"\$f\"; done" </dev/null 2>/dev/null | tr -d '\r' | LC_ALL=C sort
}
local_hashes() {  # the same for a folder holding <xuid>/465307E4/...
  # Git Bash's md5sum marks binary mode ("hash *path"); the device prints "hash  path".
  (cd "$1" && find . -path '*/465307E4/*' -type f | sed 's|^\./||' | while read -r f; do md5sum "$f"; done) |
    sed 's/^\([0-9a-f]\{32\}\) [ *]/\1  /' | LC_ALL=C sort
}
OWNER=""; OWNER_OK=0
backup_owner() {  # the save on the device now -> out/save_backups/<stamp>-bench (verified)
  # Also with AB_SAVE="" (a run on the owner's own save): the game autosaves, so a scripted run changes that save too
  # (2026-11-21: a probe run on it changed USER_DATA / USER_DATA16 until restored by hand).
  local h x
  h=$(dev_hashes)
  [ -n "$h" ] || { echo "no save on the device to back up"; return 0; }
  OWNER="out/save_backups/$(date +%Y%m%d-%H%M%S)-bench"
  mkdir -p "$OWNER"
  for x in $(echo "$h" | awk '{print $2}' | cut -d/ -f1 | sort -u); do
    mkdir -p "$OWNER/$x"
    MSYS_NO_PATHCONV=1 adb pull "$DEV_USER/$x/465307E4" "$(cygpath -m "$OWNER/$x")/" </dev/null >/dev/null || return 1
  done
  [ "$(local_hashes "$OWNER")" = "$h" ] || { echo "owner save backup does not match the device"; return 1; }
  OWNER_OK=1
  echo "owner save backed up to $OWNER"
}
restore_owner() {  # push the backed-up owner save back and check it (only a verified backup; nothing was swapped
  [ "$OWNER_OK" = 1 ] || return 0  # otherwise)
  AB_SAVE="$OWNER" push_save || { echo "WARNING: owner save NOT restored; it is in $OWNER"; return 1; }
  if [ "$(dev_hashes)" = "$(local_hashes "$OWNER")" ]; then
    echo "owner save restored from $OWNER"
  else
    echo "WARNING: the device save differs from $OWNER after the restore; push it back by hand"
  fi
}
push_save() {  # every <AB_SAVE>/<xuid>/465307E4 goes to the device's user/<xuid>/
  [ -n "$AB_SAVE" ] || return 0
  if MSYS_NO_PATHCONV=1 adb shell pidof com.ds2recomp.android </dev/null >/dev/null 2>&1; then
    echo "app running: save not pushed"
    return 1
  fi
  local prof
  for prof in "$AB_SAVE"/*/; do
    prof=${prof%/}
    [ -d "$prof/465307E4" ] || continue
    MSYS_NO_PATHCONV=1 adb push "$(cygpath -m "$prof/465307E4")"       "/sdcard/Android/data/com.ds2recomp.android/files/user/$(basename "$prof")/" </dev/null >/dev/null || return 1
  done
}
if MSYS_NO_PATHCONV=1 adb shell pidof com.ds2recomp.android com.lrrecomp.android </dev/null >/dev/null 2>&1; then
  echo "a game is running on the device: not touching it"; exit 1
fi
if MSYS_NO_PATHCONV=1 adb shell dumpsys power </dev/null | grep -q "mWakefulness=Awake" && [ "${DS2_DEVICE_OK:-0}" != 1 ]; then
  echo "device screen is on (the owner may be about to play): not swapping saves; DS2_DEVICE_OK=1 once handed over"; exit 1
fi
trap restore_owner EXIT
backup_owner || { echo "owner save backup failed: stopping before any change"; exit 1; }
export DS2_RUN_SECS=${DS2_RUN_SECS:-145}
export DS2_SCREENSHOTS=${DS2_SCREENSHOTS:-"115 130 145"}
i=0
for variant in "$@"; do
  i=$((i + 1))
  label=${variant%%|*}
  cvars=${variant#*|}
  [ "$cvars" = "$variant" ] && cvars=""
  push_save || exit 1
  # A variant may carry launch extras after a second '|': "label|cvars|--es DS2_GPU_DRIVER system".
  extras=""
  case "$cvars" in *"|"*) extras=${cvars#*|}; cvars=${cvars%%|*} ;; esac
  DS2_AM_EXTRAS="--es DS2_SAVE_SNAPSHOTS 0 $extras" DS2_CVARS="--ds2_input_script=$AB_SCRIPT $cvars" tools/android/build_android.sh launch     </dev/null >"$out/$label.txt" 2>&1 ||
    echo "$label: launch failed (see $out/$label.txt)"
  stamp=$(sed -n 's|^logs: .*/logcat_\(.*\)\.txt,.*|\1|p' "$out/$label.txt" | tail -1)
  for t in $DS2_SCREENSHOTS; do
    [ -n "$stamp" ] && [ -f "out/device/shot_${stamp}_${t}s.png" ] && mv "out/device/shot_${stamp}_${t}s.png" "$out/${i}_${label}_${t}s.png"
    for d in out/device/shot_${stamp}_${t}s_d*.png; do  # second screens (dual-screen devices)
      [ -n "$stamp" ] && [ -f "$d" ] && mv "$d" "$out/${i}_${label}_${t}s_${d##*_}"
    done
  done
  log=$( [ -n "$stamp" ] && find "out/device/logs_$stamp" -name ds2.log 2>/dev/null | head -1 || true)
  [ -n "$log" ] && cp "$log" "$out/$label.log"
  fps=$( [ -n "$log" ] && grep -a -o 'hb fps=[0-9.]*' "$log" | tail -20 | sed 's/hb fps=//' | tr '\n' ' ' || true)
  echo "$label done (last heartbeats fps: ${fps:-none})"
  # The save as the run left it (the app is stopped by now): a checkpoint for later runs
  # (AB_SAVE=<out>/<label>_end_save), e.g. after a scripted walk reached a new place.
  if [ -n "$AB_SAVE" ]; then
    for x in $(dev_hashes | awk '{print $2}' | cut -d/ -f1 | sort -u); do
      mkdir -p "$out/${label}_end_save/$x"
      MSYS_NO_PATHCONV=1 adb pull "$DEV_USER/$x/465307E4" "$(cygpath -m "$out/${label}_end_save/$x")/" </dev/null >/dev/null 2>&1 || true
    done
  fi
  # Screenshot checks (tools/android/shot_check.py): black frame, no HUD (= not in gameplay), and against AB_REF
  # (a file for a standing spot, a folder of <t>s.png for a scripted route; default <AB_SAVE>/reference.png).
  ref=${AB_REF:-}
  [ -z "$ref" ] && [ -n "$AB_SAVE" ] && [ -f "$AB_SAVE/reference.png" ] && ref="$AB_SAVE/reference.png"
  shots=""
  for t in $DS2_SCREENSHOTS; do
    f="$out/${i}_${label}_${t}s.png"
    [ -f "$f" ] || { echo "$label: screenshot at ${t}s missing"; continue; }
    if [ -d "$ref" ]; then
      [ -f "$ref/${t}s.png" ] && f="$f=$ref/${t}s.png"
    elif [ -n "$ref" ]; then
      f="$f=$ref"
    fi
    shots="$shots $f"
  done
  [ -n "$shots" ] && { python -I tools/android/shot_check.py --pairs $shots 2>&1 | sed "s/^/  $label shot: /"; } || true
done
