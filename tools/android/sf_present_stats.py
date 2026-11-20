"""On-screen frame statistics from tools/android/sf_present_sampler.sh output (SurfaceFlinger present times).

  python -I tools/android/sf_present_stats.py <sampler file> [from_s=0] [to_s=1e9] [refresh_ms=16.667]

Times are seconds from the first present in the file; negative ones count back from the last present. Prints the frames shown, the average rate, the share of
intervals longer than 18 ms (a frame shown for two or more refreshes at 60 Hz), the 1 % low (the 99th-percentile
interval as fps), intervals of 3 or more refreshes, the worst one, and a histogram in refreshes (1/2/3/4+).
"""
import sys

rows = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    p = line.split()
    if len(p) == 3:
        try:
            rows.append(int(p[1]))
        except ValueError:
            pass
rows = sorted(set(rows))
if len(rows) < 3:
    print('no presents')
    sys.exit()
frm = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
to = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
refresh = float(sys.argv[4]) if len(sys.argv) > 4 else 16.667
t0 = rows[0]
span = (rows[-1] - t0) / 1e9
if frm < 0: frm += span
if to < 0: to += span
sel = [t for t in rows if frm <= (t - t0) / 1e9 <= to]
iv = [(b - a) / 1e6 for a, b in zip(sel, sel[1:])]
# A gap of over 1 s is a pause (loading screen without presents, the sampler missing a window), not a frame.
iv = [x for x in iv if x < 1000.0]
if not iv:
    print('no intervals')
    sys.exit()
iv_sorted = sorted(iv)
p99 = iv_sorted[min(len(iv_sorted) - 1, int(0.99 * len(iv_sorted)))]
total_s = sum(iv) / 1000.0
over18 = sum(1 for x in iv if x > 18.0)
hist = [0, 0, 0, 0]
for x in iv:
    n = max(1, round(x / refresh))
    hist[min(n, 4) - 1] += 1
print(f'{len(iv) + 1} frames on screen over {total_s:.0f} s: avg {len(iv) / total_s:.1f} fps, over 18 ms '
      f'{100.0 * over18 / len(iv):.2f} %, 1% low {1000.0 / p99:.1f} fps ({p99:.1f} ms), 3+ refreshes '
      f'{hist[2] + hist[3]}, worst {iv_sorted[-1]:.0f} ms, refreshes 1/2/3/4+: {"/".join(map(str, hist))}')
