"""Compare per-second counters between slow and fast seconds of perf logs.

Usage: python -I slowfast.py START END SLOW_MAX FAST_MIN LOG [LOG ...]
Seconds with FlipsRequested <= SLOW_MAX are "slow", >= FAST_MIN are "fast". Prints counters
whose per-frame cost differs most (Ns counters as ms per second and per frame).
"""
import sys


def parse(path, start, end):
    rows = []
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
                if "=" in item:
                    key, _, value = item.partition("=")
                    try:
                        values[key] = float(value)
                    except ValueError:
                        pass
            rows.append(values)
    return rows


def main():
    start, end, slow_max, fast_min = map(float, sys.argv[1:5])
    slow, fast = [], []
    for path in sys.argv[5:]:
        for row in parse(path, start, end):
            flips = row.get("FlipsRequested", 0.0)
            if flips <= 0:
                continue
            if flips <= slow_max:
                slow.append(row)
            elif flips >= fast_min:
                fast.append(row)
    if not slow or not fast:
        print("not enough seconds", len(slow), len(fast))
        return
    keys = set()
    for row in slow + fast:
        keys.update(row)

    def mean(rows, key):
        return sum(r.get(key, 0.0) for r in rows) / len(rows)

    slow_flips, fast_flips = mean(slow, "FlipsRequested"), mean(fast, "FlipsRequested")
    print(f"slow seconds {len(slow)} ({slow_flips:.1f} flips/s), fast {len(fast)} ({fast_flips:.1f} flips/s)")
    out = []
    for key in keys:
        s, f = mean(slow, key), mean(fast, key)
        if key.endswith("Ns"):
            s_frame, f_frame = s / 1e6 / slow_flips, f / 1e6 / fast_flips
            if max(s_frame, f_frame) < 0.3:
                continue
            out.append((s_frame - f_frame, f"{key:32s} {s / 1e6:8.1f} vs {f / 1e6:8.1f} ms/s   "
                                           f"{s_frame:6.2f} vs {f_frame:6.2f} ms/frame"))
        else:
            s_frame, f_frame = s / slow_flips, f / fast_flips
            if max(s, f) < 50:
                continue
            ratio = (s_frame + 1e-9) / (f_frame + 1e-9)
            out.append(((ratio - 1.0) * 0.01, f"{key:32s} {s:10.0f} vs {f:10.0f} /s   "
                                              f"{s_frame:8.1f} vs {f_frame:8.1f} /frame (x{ratio:.2f})"))
    for _, text in sorted(out, key=lambda item: -abs(item[0]))[:45]:
        print(text)


if __name__ == "__main__":
    main()
