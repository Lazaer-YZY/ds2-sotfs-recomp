#!/usr/bin/env python3
"""Rank pixel shaders (effects) by GPU time across the traced frames in a DS2 log.

The traced frames come from vulkan_gpu_trace_frame / vulkan_gpu_trace_frames with
vulkan_gpu_trace_all_pixel_shaders (every pixel shader of the frame in draw order: draws,
primitive types, indices, GPU time, render pass signature). Each traced frame is paired with
the "GPU timing (10.0 s, N frames ...)" window it was logged with, so slow and fast windows can
be compared.

    python -I tools/android/gpu_trace_rank.py <ds2.log> [--top 30] [--since "HH:MM:SS"]
"""
import argparse
import collections
import re
import sys

ENTRY = re.compile(r'PS ([0-9A-F]{16}) VS ([0-9A-F]{16}) (\d+)x prim (\d+) n (\d+) ([\d.]+) ms in ([^|]+?)\s*(?:\||$)')
HEAD = re.compile(r'GPU traced frame, all pixel shaders \((\d+) total\):')
DRAWS = re.compile(r'GPU traced frame draws: (\d+) draws timed, ([\d.]+) ms')
TIMING = re.compile(r'GPU timing \(10\.0 s, (\d+) frames, per frame\): total ([\d.]+) ms')
STAMP = re.compile(r'^\[\d{4}-\d\d-\d\d (\d\d:\d\d:\d\d)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--since', default='')
    args = ap.parse_args()

    frames = []  # each: {'time', 'draws', 'ms', 'shaders': [(ps, vs, n, prim, idx, ms, pass)], 'fps', 'gpu_ms'}
    current = None
    last_timing = None
    with open(args.log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = STAMP.match(line)
            t = m.group(1) if m else ''
            if args.since and t and t < args.since:
                continue
            m = TIMING.search(line)
            if m:
                # The traced frame of a window is logged right after that window's timing line.
                last_timing = (int(m.group(1)) / 10.0, float(m.group(2)))
                continue
            m = DRAWS.search(line)
            if m:
                fps, gpu_ms = last_timing if last_timing else (None, None)
                current = {'time': t, 'draws': int(m.group(1)), 'ms': float(m.group(2)), 'shaders': [],
                           'fps': fps, 'gpu_ms': gpu_ms}
                frames.append(current)
                continue
            if HEAD.search(line) and current is not None:
                for e in ENTRY.finditer(line):
                    ps, vs, n, prim, idx, ms, rpass = e.groups()
                    current['shaders'].append((ps, vs, int(n), int(prim), int(idx), float(ms), rpass.strip()))
    frames = [fr for fr in frames if fr['shaders']]
    if not frames:
        print('no traced frames with the pixel shader list')
        return 1

    print(f'{len(frames)} traced frames')
    for fr in frames:
        total = sum(s[5] for s in fr['shaders'])
        print(f"  {fr['time']}: {fr['draws']} draws, {fr['ms']:.2f} ms to the end of draws, {total:.2f} ms in the "
              f"list; window {fr['fps'] or 0:.1f} fps, GPU {fr['gpu_ms'] or 0:.2f} ms/frame")

    by_ps = collections.defaultdict(lambda: {'ms': 0.0, 'draws': 0, 'idx': 0, 'frames': 0, 'passes': collections.Counter(), 'vs': collections.Counter()})
    by_pass = collections.defaultdict(float)
    for fr in frames:
        seen = set()
        for ps, vs, n, prim, idx, ms, rpass in fr['shaders']:
            d = by_ps[ps]
            d['ms'] += ms
            d['draws'] += n
            d['idx'] += idx
            d['passes'][rpass] += ms
            d['vs'][vs] += n
            if ps not in seen:
                d['frames'] += 1
                seen.add(ps)
            by_pass[rpass] += ms
    nf = len(frames)
    print(f'\nGPU ms per frame by render pass signature (average over {nf} frames):')
    for rpass, ms in sorted(by_pass.items(), key=lambda kv: -kv[1])[:12]:
        print(f'  {ms / nf:6.2f}  {rpass}')
    print(f'\nTop {args.top} pixel shaders by GPU ms per frame (average over {nf} frames; draws and indices per frame):')
    print('    ms/frame  share  draws   indices  in frames  pixel shader      main pass                main vertex shader')
    total = sum(d['ms'] for d in by_ps.values())
    for ps, d in sorted(by_ps.items(), key=lambda kv: -kv[1]['ms'])[:args.top]:
        main_pass = d['passes'].most_common(1)[0][0]
        main_vs = d['vs'].most_common(1)[0][0]
        print(f"    {d['ms'] / nf:8.3f}  {100 * d['ms'] / total:4.1f}%  {d['draws'] / nf:5.1f}  {d['idx'] / nf:8.0f}  "
              f"{d['frames']:3d}/{nf:<3d}    {ps}  {main_pass:24s} {main_vs}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
