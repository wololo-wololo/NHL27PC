import sys
from collections import defaultdict

# Usage: rbattr.py LOG START END; averages per-second readback ms by opcode over a time window.
path, start, end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
totals = defaultdict(float)
fps = []
n = 0
for line in open(path):
    if not line.startswith('t='):
        continue
    d = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    t = float(d['t'])
    if not (start <= t <= end):
        continue
    n += 1
    fps.append(float(d.get('FlipsRequested', 0)))
    for k, v in d.items():
        if k.startswith('rbop') or k.startswith('rbr'):
            totals[k] += float(v) / 1e6
        elif k in ('BufferReadBodyNs', 'GpuWaitNs', 'Pm4ProcessNs', 'DrawProcessNs', 'DispatchProcessNs', 'GpuFaultNs'):
            totals[k] += float(v) / 1e6
        elif k.startswith('op') and '/' in v:
            totals['t_' + k] += float(v.split('/')[1]) / 1e6
print('seconds', n, 'avg fps %.1f' % (sum(fps) / max(n, 1)))
for k, v in sorted(totals.items(), key=lambda kv: -kv[1])[:25]:
    print('%-20s %8.1f ms/s' % (k, v / max(n, 1)))
