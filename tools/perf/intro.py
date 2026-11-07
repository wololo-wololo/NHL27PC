"""Transition/intro summary per run: FPS, draws per frame and per-draw cost in fixed windows.

Usage: python -I intro.py LOG [LOG ...]
Windows (process time, warm autopilot runs with -SkipIntro 8): the intro after the skip taps
(t 70-106), its draw-heavy seconds (> 4,500 draws per frame) and early on-ice play (t 115-128).
"""
import os
import sys


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.replace("\0", "")
            if not line.startswith("t="):
                continue
            rows.append(dict(kv.split("=", 1) for kv in line.split() if "=" in kv))
    return rows


def value(row, key):
    try:
        return float(row.get(key, "0").split("/")[-1])
    except ValueError:
        return 0.0


def summary(rows, name):
    if not rows:
        print(f"  {name:12s} none")
        return
    n = len(rows)
    flips = sum(value(r, "FlipsRequested") for r in rows)
    draws = sum(value(r, "Draws") for r in rows)
    draw_ns = sum(value(r, "DrawProcessNs") for r in rows)
    pm4 = sum(value(r, "Pm4ProcessNs") for r in rows)
    worst = min(value(r, "FlipsRequested") for r in rows)
    print(f"  {name:12s} secs={n:2d} fps={flips / n:5.1f} min={worst:3.0f} "
          f"draws/frame={draws / max(flips, 1):5.0f} us/draw={draw_ns / max(draws, 1) / 1e3:5.2f} "
          f"pm4={pm4 / n / 1e6:4.0f} ms/s")


def main():
    for path in sys.argv[1:]:
        rows = load(path)
        print(os.path.basename(path))
        intro = [r for r in rows if 70 <= value(r, "t") <= 106]
        heavy = [r for r in intro if value(r, "FlipsRequested") > 0 and
                 value(r, "Draws") / value(r, "FlipsRequested") > 4500]
        summary(intro, "t70-106")
        summary(heavy, ">4500 dr/f")
        summary([r for r in rows if 115 <= value(r, "t") <= 128], "t115-128")


if __name__ == "__main__":
    main()
