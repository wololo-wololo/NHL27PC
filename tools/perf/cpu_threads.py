import sqlite3
import sys
from collections import Counter, defaultdict

# Per-thread CPU sample counts from an Nsight Systems export, with each busy thread's top
# exclusive and inclusive functions.
db = sqlite3.connect(sys.argv[1])
top_threads = int(sys.argv[2]) if len(sys.argv) > 2 else 8
names = dict(db.execute('select id, value from StringIds'))
thread_names = {tid: names.get(n, n) for n, tid in db.execute('select nameId, globalTid from ThreadNames')}
samples = Counter(tid for (tid,) in db.execute('select globalTid from COMPOSITE_EVENTS'))
start, end = db.execute('select min(start), max(start) from COMPOSITE_EVENTS').fetchone()
seconds = (end - start) / 1e9
rate = sum(samples.values()) / seconds / max(len(samples), 1)
print('span %.1fs, %d threads sampled' % (seconds, len(samples)))
busiest = samples.most_common(top_threads)
for tid, n in busiest:
    print('  %-32s %7d samples (%.1f/s)' % (thread_names.get(tid, hex(tid)), n, n / seconds))

event_thread = dict(db.execute('select id, globalTid from COMPOSITE_EVENTS'))
wanted = {tid for tid, _ in busiest}
exclusive = defaultdict(Counter)
inclusive = defaultdict(Counter)
seen = {}
for event, symbol, depth in db.execute('select id, symbol, stackDepth from SAMPLING_CALLCHAINS'):
    tid = event_thread.get(event)
    if tid not in wanted:
        continue
    name = names.get(symbol, str(symbol))
    if depth == 0:
        exclusive[tid][name] += 1
    key = (event, name)
    if key not in seen:
        seen[key] = True
        inclusive[tid][name] += 1
for tid, n in busiest:
    print('\n== %s (%d samples)' % (thread_names.get(tid, hex(tid)), n))
    print('  exclusive:', '; '.join('%s %.1f%%' % (k[:60], 100.0 * v / n) for k, v in exclusive[tid].most_common(8)))
    print('  inclusive:', '; '.join('%s %.1f%%' % (k[:60], 100.0 * v / n) for k, v in inclusive[tid].most_common(14)))
