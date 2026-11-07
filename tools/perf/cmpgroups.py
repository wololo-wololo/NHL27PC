"""Compare mean per-second counters between two groups of perf logs over a time window.

Usage: python -I cmpgroups.py START END A_LOG[,A_LOG...] B_LOG[,B_LOG...] [TOP]
Prints flips/s per group, then counters sorted by absolute per-second difference (Ns counters as
ms per second). Seconds without flips (loading, crashes) are skipped.
"""
import sys


def rows(path, start, end):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.replace("\0", "")
            if not line.startswith("t="):
                continue
            parts = line.split()
            try:
                t = float(parts[0][2:])
            except ValueError:
                continue
            if t < start or t > end:
                continue
            values = {}
            for item in parts[1:]:
                key, sep, value = item.partition("=")
                if not sep:
                    continue
                if "/" in value:  # opNN=calls/ns
                    calls, _, ns = value.partition("/")
                    try:
                        values[key + ".calls"] = float(calls)
                        values[key + ".Ns"] = float(ns)
                    except ValueError:
                        pass
                    continue
                try:
                    values[key] = float(value)
                except ValueError:
                    pass
            if values.get("FlipsRequested", 0) > 0:
                out.append(values)
    return out


def mean(group):
    keys = set()
    for r in group:
        keys.update(r)
    return {k: sum(r.get(k, 0.0) for r in group) / len(group) for k in keys}


def main():
    start, end = float(sys.argv[1]), float(sys.argv[2])
    top = int(sys.argv[5]) if len(sys.argv) > 5 else 60
    groups = []
    for arg in sys.argv[3:5]:
        g = []
        for path in arg.split(","):
            r = rows(path, start, end)
            print(f"{path}: {len(r)} s, flips/s {sum(x['FlipsRequested'] for x in r) / max(1, len(r)):.1f}")
            g += r
        groups.append(g)
    a, b = mean(groups[0]), mean(groups[1])
    print(f"A flips/s {a.get('FlipsRequested', 0):.2f}  B flips/s {b.get('FlipsRequested', 0):.2f}")
    diffs = []
    for k in set(a) | set(b):
        if k in ("ab",):
            continue
        va, vb = a.get(k, 0.0), b.get(k, 0.0)
        scale = 1e6 if k.endswith("Ns") else 1.0
        diffs.append((abs(vb - va) / scale, k, va / scale, vb / scale))
    diffs.sort(reverse=True)
    for d, k, va, vb in diffs[:top]:
        unit = "ms/s" if k.endswith("Ns") else "/s"
        print(f"{k:34s} A {va:12.1f}  B {vb:12.1f}  diff {vb - va:+12.1f} {unit}")


if __name__ == "__main__":
    main()
