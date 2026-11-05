import sys

# Usage: perfsum.py LOG [extra keys...]; prints one compact row per second.
path = sys.argv[1]
extra = sys.argv[2:]


def ms(d, k):
    v = d.get(k)
    if v is None:
        return 0
    if '/' in v:
        return round(float(v.split('/')[1]) / 1e6)
    return round(float(v) / 1e6)


for line in open(path):
    if not line.startswith('t='):
        continue
    d = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    row = [d['t'], 'fps=' + d.get('FlipsRequested', '0'),
           'pm4=%d' % ms(d, 'Pm4ProcessNs'), 'nop=%d' % ms(d, 'op10'),
           'disp=%d' % ms(d, 'DispatchProcessNs'), 'draw=%d' % ms(d, 'DrawProcessNs'),
           'tex=%.2fG' % (float(d.get('TextureUploadBytes', 0)) / 1e9),
           'rb=%d' % ms(d, 'BufferReadBodyNs'), 'draws=' + d.get('Draws', '0'),
           'cs=' + d.get('Dispatches', '0')]
    for k in extra:
        row.append('%s=%s' % (k, ms(d, k) if (k.startswith('op') or k.startswith('r') or k.endswith('Ns')) else d.get(k, '0')))
    print(' '.join(row))
