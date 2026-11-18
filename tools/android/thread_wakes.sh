#!/usr/bin/env bash
# Wake-ups and CPU per thread of the running game over a window (read-only): voluntary context switches per second
# (how often a thread sleeps and is woken) and % of a core. Finds threads that should run at a fixed rate (an audio
# mixer at 187.5/s = 48000/256) but don't.
#   tools/android/thread_wakes.sh [window_s=10]
set -u
export MSYS_NO_PATHCONV=1
W=${1:-10}
A=${ADB:-adb}
PID=$($A shell pidof com.ds2recomp.android </dev/null | tr -d '\r')
[ -n "$PID" ] || { echo "game not running"; exit 1; }
snap() {
  $A shell "for t in /proc/$PID/task/*; do n=\$(cat \$t/comm 2>/dev/null); v=\$(grep voluntary_ctxt_switches \$t/status 2>/dev/null | head -1 | awk '{print \$2}'); s=\$(cut -d' ' -f14,15 \$t/stat 2>/dev/null); echo \"\${t##*/}|\$n|\$v|\$s\"; done" </dev/null | tr -d '\r'
}
S1=$(snap); sleep "$W"; S2=$(snap)
python -I - "$W" <<EOF
import sys
w = float(sys.argv[1])
def parse(text):
    d = {}
    for line in text.splitlines():
        p = line.split('|')
        if len(p) < 4 or not p[2]: continue
        ut = p[3].split()
        d[p[0]] = (p[1], int(p[2]), int(ut[0]) + int(ut[1]) if len(ut) == 2 else 0)
    return d
a, b = parse('''$S1'''), parse('''$S2''')
rows = []
for tid, (name, v2, c2) in b.items():
    if tid not in a: continue
    _, v1, c1 = a[tid]
    rows.append(((v2 - v1) / w, (c2 - c1) / w, name, tid))
rows.sort(reverse=True)
print('%-6s %-18s %9s %7s' % ('tid', 'thread', 'wakes/s', 'cpu %'))
for wk, cpu, name, tid in rows[:22]:
    print('%-6s %-18s %9.1f %7.1f' % (tid, name[:18], wk, cpu))
EOF
