"""Screenshot check of a device run (the owner: every run must be checked for glitches/artifacts and for actually reaching
gameplay - the title screen, menus and loading screens also run at 60 fps).

  python -I tools/android/shot_check.py <shot.png> [<reference.png>] [--label X]
  python -I tools/android/shot_check.py --pairs <shot.png>[=<reference.png>] ...

The frame is reduced to 128x72 (tools/android/png_thumb.ps1, System.Drawing). Absolute checks, no reference needed:
  FAIL black       mean brightness under 12
  FAIL no-hud      the HUD's opaque emblem (top left, behind the health bar) is not there: title screen, menu,
                   loading/death fade, or a hidden HUD - not gameplay (template from the Things Betwixt spawn; the
                   emblem is opaque, so the background does not matter: ~2 with the HUD, 21-28 without)
With a reference of the same spot and moment (a known-good run; the scripted walk is repeatable):
  WARN artifact    a 16x9 tile differs strongly (glitch, or the camera/character is elsewhere)
  PASS             otherwise
The top-right corner (the performance overlay when on) and the CTRL button are left out of the tile comparison.
Always look at the screenshots too: these checks catch black frames, menus and big glitches, not subtle ones.
"""
import os
import subprocess
import sys

W, H = 128, 72
HERE = os.path.dirname(os.path.abspath(__file__))
# HUD boxes in 128x72 thumb coordinates (x0, y0, x1, y1) for 16:9 frames: health/stamina bars, item boxes, souls.
EMBLEM_BOX = (11, 6, 17, 11)
EMBLEM = bytes([47, 47, 47, 52, 50, 46, 29, 28, 25, 38, 36, 34, 31, 30, 28, 54, 51, 48, 49, 46, 44, 25, 24, 22, 30, 28, 26, 38,
                36, 32, 38, 35, 31, 29, 27, 24, 34, 33, 31, 29, 27, 25, 47, 44, 40, 40, 37, 33, 42, 39, 35, 37, 35, 32, 32,
                31, 29, 33, 31, 28, 45, 42, 38, 36, 34, 31, 37, 34, 31, 38, 36, 32, 44, 41, 38, 37, 35, 32, 44, 42, 39, 40,
                38, 35, 38, 36, 33, 37, 34, 30])
SKIP = [(86, 0, 128, 15), (56, 0, 72, 4)]  # overlay (top right), CTRL button (top centre)


def thumb(path):
    out = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                          os.path.join(HERE, 'png_thumb.ps1'), '-Path', os.path.abspath(path),
                          '-W', str(W), '-H', str(H)], capture_output=True, text=True, check=True).stdout.strip()
    b = bytes.fromhex(out)
    if len(b) != W * H * 3:
        raise SystemExit('thumbnail of %s: %d bytes' % (path, len(b)))
    return b


def px(b, x, y):
    i = (y * W + x) * 3
    return b[i], b[i + 1], b[i + 2]


def diff(a, b, box):
    x0, y0, x1, y1 = box
    s = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            pa, pb = px(a, x, y), px(b, x, y)
            s += (abs(pa[0] - pb[0]) + abs(pa[1] - pb[1]) + abs(pa[2] - pb[2])) / 3
            n += 1
    return s / max(n, 1)


def skipped(x, y):
    return any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in SKIP)


def emblem_diff(a):
    x0, y0, x1, y1 = EMBLEM_BOX
    total = n = 0
    k = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            pa = px(a, x, y)
            total += (abs(pa[0] - EMBLEM[k]) + abs(pa[1] - EMBLEM[k + 1]) + abs(pa[2] - EMBLEM[k + 2])) / 3
            k += 3
            n += 1
    return total / n


def check(shot, ref=None, label=''):
    a = thumb(shot)
    bright = sum(a) / len(a)
    hud = emblem_diff(a)
    tiles = ''
    worst = (0.0, None)
    if ref:
        r = thumb(ref)
        s = n = 0
        for ty in range(9):
            for tx in range(16):
                if skipped(tx * 8 + 4, ty * 8 + 4):
                    continue
                d = diff(a, r, (tx * 8, ty * 8, tx * 8 + 8, ty * 8 + 8))
                s += d
                n += 1
                if d > worst[0]:
                    worst = (d, (tx, ty))
        tiles = '  frame diff %.1f  worst tile %.1f at %s' % (s / n, worst[0], worst[1])
    if bright < 12:
        verdict = 'FAIL black'
    elif hud > 12:
        verdict = 'FAIL no-hud'
    elif worst[0] > 60:
        verdict = 'WARN artifact?'
    else:
        verdict = 'PASS'
    print('%-16s %-14s HUD %.1f  brightness %.0f%s  (%s%s)'
          % (label or os.path.basename(shot), verdict, hud, bright, tiles, os.path.basename(shot),
             ' vs ' + os.path.basename(ref) if ref else ''))
    return verdict


if __name__ == '__main__':
    args = sys.argv[1:]
    bad = 0
    if args and args[0] == '--pairs':
        for pair in args[1:]:
            shot, _, ref = pair.partition('=')
            bad += not check(shot, ref or None).startswith('PASS')
    else:
        label = ''
        if '--label' in args:
            i = args.index('--label'); label = args[i + 1]; del args[i:i + 2]
        bad += not check(args[0], args[1] if len(args) > 1 else None, label).startswith('PASS')
    sys.exit(1 if bad else 0)
