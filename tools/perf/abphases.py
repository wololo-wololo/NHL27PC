"""In-run A/B (KYTY_AB_PERIOD) by window: per-draw cost, FPS and GPU-/game-thread time sinks.

Usage: python -I abphases.py LOG [LOG ...]
Windows: intro t 60-106, its >4,500-draw seconds, early play t 106-130. The first second after
each switch is skipped. Pool several logs of the same configuration by passing them together.
"""
import sys

KEYS = ["FlipsRequested", "Pm4ProcessNs", "DrawProcessNs", "DispatchProcessNs", "ProgramMaterializeNs",
        "GpuWaitNs", "BufferReadSyncNs", "GpuFaultNs", "PrewalkHelperNs"]


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


def main():
    windows = [("intro 60-106", 60, 106, False), ("heavy", 60, 106, True), ("play 106-130", 106, 130, False)]
    groups = {(name, ab): [] for name, *_ in windows for ab in "01"}
    for path in sys.argv[1:]:
        rows = load(path)
        for name, lo, hi, heavy in windows:
            previous = None
            for row in rows:
                ab, t = row.get("ab"), value(row, "t")
                flips = value(row, "FlipsRequested")
                selected = lo <= t <= hi and ab == previous and ab in ("0", "1") and \
                    (not heavy or (flips > 0 and value(row, "Draws") / flips > 4500))
                if selected:
                    groups[(name, ab)].append(row)
                previous = ab
    for name, *_ in windows:
        for ab in "01":
            rows = groups[(name, ab)]
            if not rows:
                continue
            n = len(rows)
            draws = sum(value(r, "Draws") for r in rows)
            flips = sum(value(r, "FlipsRequested") for r in rows)
            parts = []
            for key in KEYS:
                total = sum(value(r, key) for r in rows) / n
                parts.append(f"{key[:-2]}={total / 1e6:.0f}" if key.endswith("Ns") else f"fps={total:.1f}")
            print(f"{name:13s} ab={ab} secs={n:3d} us/draw={sum(value(r, 'DrawProcessNs') for r in rows) / max(draws, 1) / 1e3:.2f} "
                  f"dr/f={draws / max(flips, 1):.0f} " + " ".join(parts))


if __name__ == "__main__":
    main()
