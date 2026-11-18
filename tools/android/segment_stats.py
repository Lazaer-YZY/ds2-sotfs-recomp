"""Benchmark report for wall-clock windows of one run: frame statistics from the game's heartbeats, GPU time from
vulkan_gpu_timing lines, and heat/power from a heat_sampler.sh CSV.

  python -I tools/android/segment_stats.py <ds2.log> <heat.csv> "<label>=HH:MM:SS-HH:MM:SS" ...

Per window: average fps, 1% low (an upper bound on the 99th-percentile frame time, from the histogram bins
<=18/21/26/34/45/more ms, as fps), frames over 34 and 45 ms, worst frame, GPU ms per frame and busy share (if
vulkan_gpu_timing was on), CPU and GPU temperature (avg/peak), GPU clock and busy (sysfs), CPU cluster clocks, and the
device's USB input power (battery full: the whole device). The log accumulates launches: only lines inside the
windows count, so windows must belong to one session.
"""
import csv
import re
import sys

BOUNDS = [18, 21, 26, 34, 45, 1000]


def hms(s):
    h, m, x = s.split(':')
    return int(h) * 3600 + int(m) * 60 + float(x)


def main():
    log, heat = sys.argv[1], sys.argv[2]
    windows = []
    for arg in sys.argv[3:]:
        label, span = arg.split('=', 1)
        a, b = span.split('-')
        windows.append((label, hms(a), hms(b)))
    hb = []
    gpu = []
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\[\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d\.\d+)\]', line)
            if not m:
                continue
            t = hms(m.group(1))
            h = re.search(r'hb fps=([0-9.]+) max_ms=(\d+) hist=([\d/]+)', line)
            if h:
                hb.append((t, float(h.group(1)), int(h.group(2)), [int(x) for x in h.group(3).split('/')]))
                continue
            g = re.search(r'GPU timing \(([0-9.]+) s, (\d+) frames, per frame\): total ([0-9.]+) ms \((\d+)% of wall', line)
            if g:
                gpu.append((t, float(g.group(3)), int(g.group(4))))
    rows = []
    with open(heat, newline='') as f:
        for r in csv.DictReader(f):
            if r.get('running') == '1':
                rows.append((hms(r['time']), r))

    for label, a, b in windows:
        # A heartbeat covers the ~2 s before it; one at or after the window start belongs to the window.
        sel = [x for x in hb if a + 1.0 <= x[0] <= b + 0.5]
        hist = [0] * 6
        for _, _, _, hh in sel:
            hist = [p + q for p, q in zip(hist, hh)]
        frames = sum(hist)
        secs = max(sel[-1][0] - sel[0][0] + 2.0, 1e-6) if sel else 1
        avg = frames / secs if sel else 0
        p99 = 0
        acc = 0
        for i, n in enumerate(hist):
            acc += n
            if acc >= frames * 0.99:
                p99 = BOUNDS[i]
                break
        worst = max((x[2] for x in sel), default=0)
        g = [x for x in gpu if a + 5 <= x[0] <= b + 5]
        hr = [r for t, r in rows if a <= t <= b]

        def st(key, scale=1.0):
            v = [float(r[key]) * scale for r in hr if r.get(key) not in (None, '') and float(r[key]) > 0]
            return (sum(v) / len(v), max(v)) if v else (0.0, 0.0)

        cpu, gpu_c = st('cpu_c'), st('gpu_c')
        mhz, busy, usb = st('gpu_mhz'), st('gpu_busy'), st('usb_w')
        c0, c3, c7 = st('cpu0_khz', 1e-6), st('cpu3_khz', 1e-6), st('cpu7_khz', 1e-6)
        print(f'== {label} ({int(b - a)} s, {len(sel)} heartbeats, {len(hr)} heat samples)')
        print(f'  fps avg {avg:.1f}, 1% low >= {1000.0 / p99 if p99 else 0:.1f} fps (99% of frames <= {p99} ms), '
              f'frames > 34 ms: {hist[4] + hist[5]}, > 45 ms: {hist[5]}, worst {worst} ms; '
              f'histogram <=18/21/26/34/45/more: {"/".join(map(str, hist))}')
        if g:
            print(f'  GPU work {sum(x[1] for x in g) / len(g):.2f} ms/frame ({sum(x[2] for x in g) / len(g):.0f}% of wall '
                  f'time, {len(g)} x 10 s windows)')
        print(f'  CPU {cpu[0]:.1f}/{cpu[1]:.1f} C, GPU {gpu_c[0]:.1f}/{gpu_c[1]:.1f} C (avg/peak); GPU {mhz[0]:.0f} MHz '
              f'{busy[0]:.0f}% busy; clocks little {c0[0]:.2f} middle {c3[0]:.2f} prime {c7[0]:.2f} GHz; '
              f'power {usb[0]:.2f} W avg, {usb[1]:.2f} W peak')


if __name__ == '__main__':
    main()
