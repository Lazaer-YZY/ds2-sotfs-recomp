# Attribute vulkan_gpu_timing windows to live settings: for a run whose settings changed live (live_cvars.txt, logged
# as "[ds2 wd] live setting name=value: applied"), list each 10-s "GPU timing" window that lies wholly inside one
# settings step (from 2 s after the step's first applied line to the next step), with the step's label from a
# schedule file ("HH:MM:SS step N label" lines) and the watchdog's GPU clock nearest to the window's end.
#   python -I tools/android/subtract_windows.py <game log> <schedule.txt>
import re
import sys
from datetime import datetime, timedelta


def ts(line):
    m = re.match(r"\S+\s+(\d\d:\d\d:\d\d(?:\.\d+)?)", line)
    if not m:
        return None
    t = m.group(1)
    return datetime.strptime(t[:15] if "." in t else t, "%H:%M:%S.%f" if "." in t else "%H:%M:%S")


def main():
    log, sched = sys.argv[1], sys.argv[2]
    labels = []
    with open(sched, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) >= 4 and p[1] == "step":
                labels.append(p[3])
    applies, windows, clocks = [], [], []
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            t = ts(line)
            if t is None:
                continue
            if "live setting" in line:
                ok = "applied" in line
                if not applies or (t - applies[-1][0]).total_seconds() > 3:
                    applies.append([t, ok, line.split("live setting", 1)[1].strip()[:120]])
                else:
                    applies[-1][1] = applies[-1][1] and ok
                    applies[-1][2] += " | " + line.split("live setting", 1)[1].strip()[:120]
            m = re.search(r"GPU timing \(([\d.]+) s, (\d+) frames, per frame\): total ([\d.]+) ms .*?= draws ([\d.]+) "
                          r"\+ transfers ([\d.]+) \+ resolves ([\d.]+).*?per frame ([\d.]+) render passes.*?"
                          r"([\d.]+) transfers, ([\d.]+) resolves.*?([\d.]+) draws", line)
            if m:
                windows.append((t, float(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4)),
                                float(m.group(5)), float(m.group(6)), float(m.group(7)), float(m.group(10))))
            m = re.search(r"gpu (\d+)/(\d+) thermal", line)
            if m:
                clocks.append((t, int(m.group(1))))
    print(f"{len(applies)} steps applied, {len(labels)} scheduled, {len(windows)} timing windows")
    rows = {}
    for i, (t0, ok, text) in enumerate(applies):
        t1 = applies[i + 1][0] if i + 1 < len(applies) else t0 + timedelta(seconds=40)
        label = labels[i] if i < len(labels) else f"step{i}"
        for (te, secs, frames, total, draws, xfer, res, passes, ndraws) in windows:
            if te - timedelta(seconds=secs) >= t0 + timedelta(seconds=2) and te <= t1:
                mhz = min(clocks, key=lambda c: abs((c[0] - te).total_seconds()))[1] if clocks else 0
                fps = frames / secs
                print(f"{i:2d} {label:18s} {te.strftime('%H:%M:%S')} {fps:5.1f} fps  total {total:6.2f} ms  "
                      f"draws {draws:5.2f} xfer {xfer:4.2f} res {res:4.2f}  passes {passes:5.1f} draws# {ndraws:6.1f}  "
                      f"{mhz} MHz  {total * mhz / 1000:6.2f} Mcyc/frame{'' if ok else '  (NOT APPLIED)'}")
                rows.setdefault(label, []).append(total * mhz / 1000 if mhz else total)
    print("\nper label: mean Mcycles (or ms) per frame over its windows")
    for label, v in rows.items():
        print(f"  {label:18s} {sum(v) / len(v):6.2f}  ({len(v)} windows)")


if __name__ == "__main__":
    main()
