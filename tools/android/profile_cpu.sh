#!/usr/bin/env bash
# CPU profile (simpleperf) of the game while it already runs, e.g. beside an ab_run.sh variant.
# Read-only: never wakes, starts or stops anything (the app is debuggable, so simpleperf --app works).
#   tools/android/profile_cpu.sh <out prefix> [seconds=10]
# Env: CG=<thread name part> also records call graphs of that thread for 4 s (CGMODE=fp|dwarf, default fp),
# reported by caller for the symbols in SYM (default: the whole thread).
# Output: <out prefix>.txt (by thread+library, then thread+library+symbol), .threads.txt, .cg.txt.
# Adapted from the LR port's tools/thor_profile.sh.
set -u
export MSYS_NO_PATHCONV=1
F=${1:?out prefix}; SECS=${2:-10}
PKG=com.ds2recomp.android
A=${ADB:-adb}
mkdir -p "$(dirname "$F")"
if [ -z "$($A shell pidof $PKG </dev/null | tr -d '\r')" ]; then echo "game not running"; exit 1; fi
$A shell "cd /data/local/tmp && rm -f ds2.perf && simpleperf record --app $PKG --duration $SECS -f 2000 -o ds2.perf" </dev/null >/dev/null 2>&1
{ echo "== by thread, library"
  $A shell "cd /data/local/tmp && simpleperf report -i ds2.perf --sort comm,dso 2>/dev/null" </dev/null | tr -d '\r' | head -45
  echo "== by thread, library, symbol"
  $A shell "cd /data/local/tmp && simpleperf report -i ds2.perf --sort comm,dso,symbol 2>/dev/null" </dev/null | tr -d '\r' | head -200; } > "$F.txt"
$A shell "top -H -b -n 1 -p \$(pidof $PKG) | head -25" </dev/null | tr -d '\r' > "$F.threads.txt"
if [ -n "${CG:-}" ]; then
  tid=$($A shell "for t in /proc/\$(pidof $PKG)/task/*; do n=\$(cat \$t/comm); case \"\$n\" in *\"$CG\"*) basename \$t; break;; esac; done" </dev/null | tr -d '\r')
  if [ -n "$tid" ]; then
    $A shell "cd /data/local/tmp && rm -f ds2_cg.perf && simpleperf record --app $PKG -t $tid --call-graph ${CGMODE:-fp} --duration 4 -o ds2_cg.perf" </dev/null >/dev/null 2>&1
    sym=""; [ -n "${SYM:-}" ] && sym="--symbols '$SYM'"
    $A shell "cd /data/local/tmp && simpleperf report -i ds2_cg.perf -g caller --percent-limit 1 $sym 2>/dev/null" </dev/null | tr -d '\r' | head -300 > "$F.cg.txt"
    echo "call graph of thread $tid ($CG): $F.cg.txt"
  fi
fi
echo "profile: $F.txt"
grep -E '^ *[0-9.]+%' "$F.txt" | head -16 | tr -s ' ' | cut -c1-150
