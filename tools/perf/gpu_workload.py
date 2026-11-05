import sqlite3
import sys
from collections import Counter, defaultdict

# Summarizes Nsight Systems Vulkan GPU workloads: GPU busy time (union), workload kinds by
# name, and the longest individual workloads.
db = sqlite3.connect(sys.argv[1])
names = dict(db.execute('select id, value from StringIds'))
rows = db.execute('select start, end, nameId, correlationId from VULKAN_WORKLOAD order by start').fetchall()
if not rows:
    sys.exit('no workloads')
span = rows[-1][1] - rows[0][0]
busy = 0
cur_s, cur_e = rows[0][0], rows[0][1]
for s, e, _, _ in rows[1:]:
    if s > cur_e:
        busy += cur_e - cur_s
        cur_s, cur_e = s, e
    else:
        cur_e = max(cur_e, e)
busy += cur_e - cur_s
print('workloads=%d span=%.2fs gpu_busy=%.1f%%' % (len(rows), span / 1e9, 100.0 * busy / span))
total = defaultdict(int)
count = Counter()
for s, e, n, _ in rows:
    total[names.get(n, n)] += e - s
    count[names.get(n, n)] += 1
print('\nby name (sum of durations, ms per second):')
for name, t in sorted(total.items(), key=lambda kv: -kv[1])[:15]:
    print('  %8.1f ms/s  %8d  %s' % (t / 1e6 / (span / 1e9), count[name], name))
print('\nlongest workloads:')
api = {}
for cid, n in db.execute('select correlationId, nameId from VULKAN_API'):
    api[cid] = names.get(n, n)
for s, e, n, cid in sorted(rows, key=lambda r: r[0] - r[1])[:15]:
    print('  %8.2f ms  at %.3fs  %s  (api: %s)' % ((e - s) / 1e6, (s - rows[0][0]) / 1e9,
                                                   names.get(n, n), api.get(cid, '?')))
hist = Counter()
for s, e, _, _ in rows:
    d = (e - s) / 1e3
    hist['<10us' if d < 10 else '<100us' if d < 100 else '<1ms' if d < 1000 else '<10ms' if d < 10000 else '>=10ms'] += 1
print('\nduration histogram:', dict(hist))
