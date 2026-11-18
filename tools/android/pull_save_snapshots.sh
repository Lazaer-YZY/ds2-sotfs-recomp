#!/usr/bin/env bash
# Copy the development build's save snapshots (app/src/android/ds2_save_snapshots.cpp) from the device to
# out/save_snapshots/<YYYYMMDD-HHMMSS>/ (git-ignored: game saves). Read-only on the device; already pulled ones are
# skipped. A bench from one: AB_SAVE=out/save_snapshots/<stamp> tools/android/ab_run.sh ...
set -u
cd "$(dirname "$0")/../.."
source tools/android/env.sh
export MSYS_NO_PATHCONV=1
REMOTE=/sdcard/Android/data/com.ds2recomp.android/files/save_snapshots
mkdir -p out/save_snapshots
n=0
for s in $(adb shell "ls '$REMOTE' 2>/dev/null" </dev/null | tr -d '\r' | grep -E '^[0-9]{8}-[0-9]{6}$'); do
  [ -d "out/save_snapshots/$s" ] && continue
  adb pull "$REMOTE/$s" "$(cygpath -m out/save_snapshots)/" </dev/null >/dev/null && n=$((n + 1)) && echo "pulled $s"
done
echo "$n new snapshot(s); out/save_snapshots has $(ls -d out/save_snapshots/*/ 2>/dev/null | wc -l)"
