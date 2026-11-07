"""Look-ahead materialization feasibility (KYTY_PREWALK_PROBE=1 runs).

Usage: python -I prewalk.py LOG [LOG ...]
Shares of materialization time per category for menus, the intro, its draw-heavy seconds and
early play, plus the reasons plans were ineligible for a walk at slice start.
"""
import os
import sys

CATS = ["PrewalkValid", "PrewalkIneligible", "PrewalkFaulted", "PrewalkSliceWritten"]
REASONS = ["PrewalkNoWalker", "PrewalkCapturedReads", "PrewalkSourceReads", "PrewalkSelectorKeys",
           "PrewalkIndirect"]


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
    total = sum(value(r, c + "Ns") for r in rows for c in CATS)
    materialize = sum(value(r, "ProgramMaterializeNs") for r in rows) / len(rows) / 1e6
    shares = " ".join(f"{c[7:]} {100 * sum(value(r, c + 'Ns') for r in rows) / max(total, 1):4.1f}%"
                      for c in CATS)
    print(f"  {name:16s} secs={len(rows):2d} materialize={materialize:4.0f} ms/s  {shares}")
    reasons = " ".join(f"{c[7:]} {100 * sum(value(r, c + 'Ns') for r in rows) / max(total, 1):4.1f}%"
                       for c in REASONS)
    print(f"  {'':16s} ineligible by reason: {reasons}")
    captured_valid = sum(value(r, "PrewalkCapturedValidNs") for r in rows)
    print(f"  {'':16s} captured-read plans valid at slice start: "
          f"{100 * captured_valid / max(total, 1):4.1f}% of all materialization time")


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
