# Per-variant GPU cost from an ab_run.sh folder whose launches had DS2_GPU_COUNTERS=1 (and vulkan_gpu_timing): the
# last N 10-s counter windows of each <label>.log (gameplay at the end of the run), as GPU busy Mcycles per frame
# (cp_busy / fps: independent of the governor's clock), shader-processor busy, VFD busy and SP stalls on the texture
# pipe per frame, and the GPU timing windows' ms per frame and clock.
#   python -I tools/android/ab_counters.py <ab dir> <label> ... [--last N]
import re
import sys


def main():
    args = sys.argv[1:]
    last = 3
    if "--last" in args:
        i = args.index("--last")
        last = int(args[i + 1])
        del args[i:i + 2]
    folder, labels = args[0], args[1:]
    print(f"{'variant':10s} {'cp_busy':>8s} {'sp_busy':>8s} {'vfd':>6s} {'sp_st_tp':>8s} {'tp_l1miss':>9s} "
          f"{'gpu ms':>7s} {'fps':>5s}   (Mcycles per frame, last {last} windows)")
    for label in labels:
        try:
            text = open(f"{folder}/{label}.log", encoding="utf-8", errors="replace").read()
        except OSError:
            print(f"{label:10s} no log")
            continue
        raw = re.findall(r"ds2 GPU counters \(([\d.]+) s, per second\): (.*)", text)
        derived = re.findall(r"tp l1 miss ([\d.]+)%", text)
        timing = re.findall(r"GPU timing \(([\d.]+) s, (\d+) frames, per frame\): total ([\d.]+) ms", text)
        if not raw or not timing:
            print(f"{label:10s} no counters/timing ({len(raw)} / {len(timing)})")
            continue
        rows = []
        for secs, body in raw[-last:]:
            vals = dict((k, float(v)) for k, v in re.findall(r"(\w+) ([\d.]+)M", body))
            rows.append(vals)
        fps = [int(f) / float(s) for s, f, _ in timing[-last:]]
        ms = [float(t) for _, _, t in timing[-last:]]
        f = sum(fps) / len(fps)

        def per_frame(key):
            v = [r.get(key, 0.0) for r in rows]
            return sum(v) / len(v) / f

        miss = sum(float(x) for x in derived[-last:]) / max(len(derived[-last:]), 1)
        print(f"{label:10s} {per_frame('cp_busy'):8.2f} {per_frame('sp_busy'):8.2f} {per_frame('vfd_busy'):6.2f} "
              f"{per_frame('sp_stall_tp'):8.2f} {miss:8.1f}% {sum(ms) / len(ms):7.2f} {f:5.1f}")


if __name__ == "__main__":
    main()
