import sys

# Usage: keys.py LOG START END KEY...; prints raw per-second values of the keys.
path, start, end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
keys = sys.argv[4:]
for line in open(path):
    if not line.startswith('t='):
        continue
    d = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    if start <= float(d['t']) <= end:
        print(d['t'], ' '.join('%s=%s' % (k, d.get(k, '-')) for k in keys))
