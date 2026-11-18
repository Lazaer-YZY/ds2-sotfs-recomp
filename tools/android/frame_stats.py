"""Frame statistics of a ds2.log from the heartbeat histograms (app/src/ds2_watchdog.cpp).

  python -I tools/android/frame_stats.py <ds2.log> [from_s=110] [to_s=99999]

Heartbeats every 2 s carry the game's presented frames of the last ~2 s as fps, max_ms and hist=a/b/c/d/e/f
(frames <= 18 / 21 / 26 / 34 / 45 ms / longer). Prints the average fps, the 1% low (the frame time below which
99 % of frames fall, as fps, from the histogram bin bounds: an upper bound on the frame time), frames over 45 ms
and the worst frame. Only the last session in the log (it accumulates launches).
"""
import re
import sys

text = open(sys.argv[1], errors='replace').read()
text = text[max(0, text.rfind('Available Vulkan physical')):]
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 110.0
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
bounds = [18.0, 21.0, 26.0, 34.0, 45.0, None]
hist = [0] * 6
fps, worst = [], 0.0
for m in re.finditer(r't=(\d+)s hb fps=([0-9.]+) max_ms=([0-9.]+) hist=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)', text):
    t = float(m.group(1))
    if t < lo or t > hi:
        continue
    fps.append(float(m.group(2)))
    worst = max(worst, float(m.group(3)))
    for i in range(6):
        hist[i] += int(m.group(4 + i))
total = sum(hist)
if not total:
    sys.exit('no heartbeats with a histogram in the window')
acc, low_ms = 0, None
for i, n in enumerate(hist):
    acc += n
    if acc >= 0.99 * total:
        low_ms = bounds[i] if bounds[i] else worst
        break
print('frames %d, avg %.1f fps, 1%% low >= %.1f fps (99 %% of frames <= %.0f ms), over 45 ms: %d, worst %.0f ms, '
      'histogram <=18/21/26/34/45/more: %s' % (total, sum(fps) / len(fps), 1000.0 / low_ms, low_ms, hist[5], worst,
                                              '/'.join(map(str, hist))))
