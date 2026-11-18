#!/usr/bin/env bash
# Per-thread CPU of the running game over a window: % of one core and ms per frame at the given frame rate.
#   tools/android/thread_cpu.sh [window_s=30] [fps=60]
# Read-only (/proc of the app's threads through run-as is not needed: adb shell can read /proc/<pid>/task/*/stat
# of a debuggable app's process on this device). Prints the 10 busiest threads and the process total.
set -u
export MSYS_NO_PATHCONV=1
W=${1:-30}; FPS=${2:-60}
A=${ADB:-adb}
PID=$($A shell pidof com.ds2recomp.android </dev/null | tr -d '\r')
[ -n "$PID" ] || { echo "game not running"; exit 1; }
snap() { $A shell "for t in /proc/$PID/task/*; do s=\$(cat \$t/stat 2>/dev/null) && echo \"\${t##*/} \$s\"; done" </dev/null | tr -d '\r'; }
S1=$(snap); sleep "$W"; S2=$(snap)
python -I - "$W" "$FPS" <<EOF
import sys, re
w, fps = float(sys.argv[1]), float(sys.argv[2])
def parse(text):
    d = {}
    for line in text.splitlines():
        m = re.match(r'(\d+) \d+ \((.*)\) (.*)', line)
        if not m: continue
        f = m.group(3).split()
        d[m.group(1)] = (m.group(2), int(f[11]) + int(f[12]))  # utime + stime (ticks, 100 Hz)
    return d
a, b = parse('''$S1'''), parse('''$S2''')
rows = []
for tid, (name, t2) in b.items():
    t1 = a.get(tid, (name, t2))[1]
    rows.append((t2 - t1, name, tid))
rows.sort(reverse=True)
total = sum(r[0] for r in rows)
print("window %.0f s, %d threads; process total %.0f%% of a core" % (w, len(rows), total / w))
for ticks, name, tid in rows[:10]:
    pct = ticks / w
    print("  %-28s %5.1f%%  %5.2f ms/frame" % (name[:28], pct, pct / 100 * 1000 / fps))
EOF
