"""Look-ahead helper statistics (KYTY_PREWALK=1 runs).

Usage: python -I prewalkrun.py LOG [LOG ...]
Per window: share of GPU-thread materializations answered by the helper, the reasons for the
rest, helper jobs/busy time, verify mismatches, and the GPU thread's materialization cost.
"""
import os
import sys

TAKE = ["PrewalkTaken", "PrewalkNoMatch", "PrewalkNotReady", "PrewalkFailed", "PrewalkRejected"]


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.replace("\0", "")
            if line.startswith("t="):
                rows.append(dict(kv.split("=", 1) for kv in line.split() if "=" in kv))
    return rows


def value(row, key):
    try:
        return float(row.get(key, "0").split("/")[-1])
    except ValueError:
        return 0.0


def window(name, rows):
    if not rows:
        return
    n = len(rows)
    total = sum(value(r, k) for r in rows for k in TAKE)
    shares = " ".join(f"{k[7:]} {100 * sum(value(r, k) for r in rows) / max(total, 1):4.1f}%" for k in TAKE)
    per = lambda k: sum(value(r, k) for r in rows) / n
    flips = per("FlipsRequested")
    print(f"  {name:14s} secs={n:2d} fps={flips:5.1f} lookups/s={per('ProgramLookups'):7.0f}  {shares}")
    print(f"  {'':14s} jobs/s={per('PrewalkJobs'):7.0f} helper={per('PrewalkHelperNs') / 1e6:4.0f} ms/s "
          f"failed/s={per('PrewalkHelperFailed'):5.0f} gpu-dirty/s={per('PrewalkHelperGpuDirty'):5.0f} "
          f"stops/s={per('PrewalkScanStops'):5.0f} lost/s={per('PrewalkScanLost'):3.0f} "
          f"verify-mismatch={sum(value(r, 'PrewalkVerifyMismatches') for r in rows):.0f}")
    print(f"  {'':14s} GPU thread: materialize={per('ProgramMaterializeNs') / 1e6:4.0f} ms/s "
          f"walk={per('WalkSrtNs') / 1e6:4.0f} ms/s pm4={per('Pm4ProcessNs') / 1e6:4.0f} ms/s "
          f"draws/frame={per('Draws') / max(flips, 1):5.0f} us/draw="
          f"{sum(value(r, 'DrawProcessNs') for r in rows) / max(sum(value(r, 'Draws') for r in rows), 1) / 1e3:5.2f}")


def main():
    for path in sys.argv[1:]:
        rows = load(path)
        print(os.path.basename(path))
        window("menus t20-36", [r for r in rows if 20 <= value(r, "t") <= 36])
        intro = [r for r in rows if 70 <= value(r, "t") <= 106]
        window("intro t70-106", intro)
        window("heavy seconds", [r for r in intro if value(r, "FlipsRequested") > 0 and
                                 value(r, "Draws") / value(r, "FlipsRequested") > 4500])
        window("play t110-130", [r for r in rows if 110 <= value(r, "t") <= 130])


if __name__ == "__main__":
    main()
