import statistics
import sys

# Usage: abstat.py LOG START END [KEY...]; compares per-second means between ab=0 and ab=1,
# skipping the first second after each switch.
path, start, end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
keys = sys.argv[4:] or ['FlipsRequested', 'Draws', 'Dispatches', 'Pm4ProcessNs', 'GpuWaitNs',
                        'BufferReadCalls', 'BufferReadBodyNs']
groups = {'0': [], '1': []}
previous = None
for line in open(path):
    if not line.startswith('t='):
        continue
    d = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    t = float(d['t'])
    ab = d.get('ab')
    if ab is None or not (start <= t <= end):
        previous = ab
        continue
    if ab == previous:
        groups[ab].append(d)
    previous = ab
for key in keys:
    row = []
    for ab in ('0', '1'):
        values = [float(d.get(key, '0').split('/')[-1]) for d in groups[ab]]
        if key.endswith('Ns'):
            values = [v / 1e6 for v in values]
        mean = statistics.mean(values) if values else 0.0
        err = statistics.stdev(values) / len(values) ** 0.5 if len(values) > 1 else 0.0
        row.append('%10.1f +- %6.1f' % (mean, err))
    print('%-20s off: %s   on: %s' % (key, row[0], row[1]))
print('seconds: off=%d on=%d' % (len(groups['0']), len(groups['1'])))
