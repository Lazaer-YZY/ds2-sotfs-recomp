# Symbolize a profile_cpu.sh report whose libraries were stripped on the device: every "<lib>[+offset]" is looked up
# in the unstripped copy of that library (llvm-symbolizer) and replaced by its function name. With --thread, also sums
# the self time of that thread by function (all libraries), largest first.
#   python -I tools/android/symbolize_profile.py <report.txt> <unstripped lib.so> [--thread "GPU Commands"] [--top 40]
# Writes <report>.sym.txt next to the report (the call graph keeps its shape, names substituted).
import argparse
import collections
import re
import subprocess
import sys

SYMBOLIZER = 'C:/dev/llvm/bin/llvm-symbolizer.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report')
    ap.add_argument('lib')
    ap.add_argument('--thread')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    libname = a.lib.replace('\\', '/').split('/')[-1]
    text = open(a.report, encoding='utf-8', errors='replace').read()
    rx = re.compile(re.escape(libname) + r'\[\+([0-9a-f]+)\]')
    offsets = sorted(set(rx.findall(text)))
    names = {}
    if offsets:
        inp = '\n'.join('0x' + o for o in offsets) + '\n'
        out = subprocess.run([SYMBOLIZER, '--obj=' + a.lib, '--functions=short', '--no-inlines', '--demangle'],
                             input=inp, capture_output=True, text=True).stdout
        blocks = [b for b in out.strip().split('\n\n')]
        for o, b in zip(offsets, blocks):
            fn = b.split('\n')[0].strip()
            names[o] = fn if fn and fn != '??' else libname + '[+' + o + ']'
    sym = rx.sub(lambda m: names.get(m.group(1), m.group(0)), text)
    out_path = a.report.rsplit('.', 1)[0] + '.sym.txt'
    with open(out_path, 'w', encoding='utf-8', newline='') as f:
        f.write(sym)
    print('symbolized', len(names), 'addresses ->', out_path)
    if a.thread:
        # Self-time rows: "<pct>% <thread...> <dso> <symbol>"
        totals = collections.Counter()
        for line in sym.splitlines():
            m = re.match(r'\s*([0-9.]+)%\s+(.*)$', line)
            if not m or a.thread not in m.group(2):
                continue
            rest = m.group(2)
            parts = rest.split()
            if len(parts) < 4:
                continue
            # the symbol is everything after the dso path
            dso_idx = next((i for i, p in enumerate(parts) if '/' in p or p.startswith('[')), None)
            if dso_idx is None or dso_idx + 1 >= len(parts):
                continue
            fn = ' '.join(parts[dso_idx + 1:])
            fn = re.sub(r'\(.*$', '', fn)
            totals[fn] += float(m.group(1))
        tot = sum(totals.values())
        print(f'{a.thread}: {tot:.2f}% of all samples in symbolized rows')
        for fn, p in totals.most_common(a.top):
            print(f'{p:6.2f}%  {100 * p / tot:5.1f}% of thread  {fn[:150]}')


if __name__ == '__main__':
    sys.exit(main())
