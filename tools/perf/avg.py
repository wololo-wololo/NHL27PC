import sys
from collections import defaultdict

# Usage: avg.py LOG START END KEY...; averages per-second counter rates over a time window.
path, start, end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
keys = sys.argv[4:]
totals = defaultdict(float)
n = 0
for line in open(path):
    if not line.startswith('t='):
        continue
    d = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    if not (start <= float(d['t']) <= end):
        continue
    n += 1
    for k in keys:
        v = d.get(k, '0')
        totals[k] += float(v.split('/')[-1])
for k in keys:
    v = totals[k] / max(n, 1)
    print('%-26s %14.1f%s' % (k, v / 1e6 if k.endswith('Ns') or k.startswith(('op', 'rb')) else v,
                              ' ms/s' if k.endswith('Ns') or k.startswith(('op', 'rb')) else '/s'))
