"""heat_window.py <sampler csv> <from_s> <to_s>: means over rows from..to seconds after the game was first seen running:
USB power (W), CPU / GPU temperature (avg, peak), GPU MHz / busy, cpu3 / cpu7 MHz."""
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], encoding='utf-8')))
t0 = next((i for i, r in enumerate(rows) if r['running'] == '1'), None)
if t0 is None: print('no game rows'); sys.exit()
# time column is HH:MM:SS
def sec(r):
    h, m, s = (int(x) for x in r['time'].split(':')); return h * 3600 + m * 60 + s
base = sec(rows[t0]); a, b = float(sys.argv[2]), float(sys.argv[3])
w = [r for r in rows[t0:] if a <= sec(r) - base <= b and r['running'] == '1']
def num(x):
    # Some sensors print values the sampler's arithmetic mangles (the S10's GPU sensor: "0.-6"): skipped.
    try:
        return float(x)
    except (TypeError, ValueError):
        return None
def values(k):
    return [v for v in (num(r.get(k)) for r in w) if v is not None]
def avg(k):
    v = values(k)
    return sum(v) / len(v) if v else float('nan')
def peak(k):
    v = values(k)
    return max(v) if v else float('nan')
print(f'{len(w)} samples: {avg("usb_w"):.2f} W, CPU {avg("cpu_c"):.1f}/{peak("cpu_c"):.1f} C, GPU {avg("gpu_c"):.1f}/{peak("gpu_c"):.1f} C, '
      f'GPU {avg("gpu_mhz"):.0f} MHz {avg("gpu_busy"):.0f} %, cpu3 {avg("cpu3_khz")/1000:.0f} MHz, cpu7 {avg("cpu7_khz")/1000:.0f} MHz')
