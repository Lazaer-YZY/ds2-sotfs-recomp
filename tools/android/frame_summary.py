"""Frame breakdown summary of a ds2.log (debug logging on: frame_breakdown_ms, sdk xboxkrnl_video.cpp).

  python -I tools/android/frame_summary.py <ds2.log> <from_s>

The last launch's 10-s frame breakdown summaries from <from_s> seconds after the launch: means per frame of the game
thread's cpu / run queue / waits / frame cap and the process CPU, frames over 20 ms and the worst one; the heartbeat
histogram (frames <= 18 / 21 / 26 / 34 / 45 ms / longer); the job thread groups' CPU / run-queue time per frame and the
average wait for a core per run (tens of us: wake-up latency; milliseconds: queued behind other threads); CPU busy %
per core; the last prime-core switches.
"""
import re, sys, datetime
lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
start = max(i for i, l in enumerate(lines) if 'defaults applied' in l)
lines = lines[start:]
def ts(l):
    m = re.match(r'\[(\d+-\d+-\d+ [\d:.]+)\]', l)
    return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f') if m else None
t0 = ts(lines[0]); frm = float(sys.argv[2])
rows = []
for l in lines:
    if 'frame breakdown (10 s)' not in l: continue
    t = ts(l)
    if (t - t0).total_seconds() < frm: continue
    m = re.search(r'(\d+) frames, avg ([\d.]+) ms \(max ([\d.]+), (\d+) over 20 ms\) = cpu ([\d.]+) \+ run queue ([\d.]+) \+ waits ([\d.]+) \+ delays ([\d.]+) \+ frame cap ([\d.]+) \+ other ([\d.-]+); process cpu ([\d.]+)', l)
    if m: rows.append([float(x) for x in m.groups()])
hist = [0] * 6
for l in lines:
    if 'hb fps=' not in l: continue
    m = re.search(r't=(\d+)s hb fps=[\d.]+ max_ms=\d+ hist=([\d/]+)', l)
    if m and int(m.group(1)) >= frm:
        for i, x in enumerate(m.group(2).split('/')): hist[i] += int(x)
if not rows: print('no summaries'); sys.exit()
n = len(rows)
avg = lambda k: sum(r[k] for r in rows) / n
tot = sum(hist) or 1
print(f'{n} x 10 s: cpu {avg(4):.2f} rq {avg(5):.2f} waits {avg(6):.2f} cap {avg(8):.2f} other {avg(9):.2f} proc {avg(10):.1f} | '
      f'over20 {sum(r[3] for r in rows):.0f}, max {max(r[2] for r in rows):.0f} ms | hist {"/".join(map(str, hist))} '
      f'(>18 ms {100 * (tot - hist[0]) / tot:.1f} %)')
groups = {}
busy = {}
for l in lines:
    if 'threads cpu/run queue per frame' not in l: continue
    t = ts(l)
    if (t - t0).total_seconds() < frm: continue
    for m in re.finditer(r'(workers|havok|draw|gpu) ([\d.]+)/([\d.]+)(?: \(([\d.]+) runs, ([\d.]+) us)?', l):
        groups.setdefault(m.group(1), []).append(
            (float(m.group(2)), float(m.group(3)), float(m.group(4) or 0), float(m.group(5) or 0)))
    m = re.search(r'cpu busy %: (.*)$', l)
    if m:
        for c in m.group(1).split():
            k, _, v = c.partition(':')
            if v.isdigit(): busy.setdefault(int(k), []).append(int(v))
if groups:
    print('   threads cpu/run queue per frame (runs, us queued per run): ' + ', '.join(
        f'{k} {sum(x[0] for x in v) / len(v):.2f}/{sum(x[1] for x in v) / len(v):.2f}'
        f' ({sum(x[2] for x in v) / len(v):.0f}, {sum(x[3] for x in v) / len(v):.0f})' for k, v in groups.items()))
if busy:
    print('   cpu busy %: ' + ' '.join(f'{k}:{sum(v) / len(v):.0f}' for k, v in sorted(busy.items())))
prime = [l for l in lines if 'ds2 prime core:' in l]
if prime: print('   ' + ' | '.join(l.split('ds2 prime core: ')[1][:70] for l in prime[-4:]))
