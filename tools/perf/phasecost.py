"""Per-draw / per-frame cost breakdown for time windows of one perf log.

Usage: python -I phasecost.py LOG NAME:START-END [NAME:START-END ...]
For each window prints flips/s, draws per frame, and each Ns counter as microseconds per draw
(for draw-path counters) and ms per frame, sorted by ms per frame.
"""
import sys


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.replace("\0", "")
            if not line.startswith("t="):
                continue
            d = {}
            for item in line.split():
                k, sep, v = item.partition("=")
                if not sep:
                    continue
                if "/" in v:
                    c, _, n = v.partition("/")
                    try:
                        d[k + ".calls"] = float(c)
                        d[k + ".Ns"] = float(n)
                    except ValueError:
                        pass
                    continue
                try:
                    d[k] = float(v)
                except ValueError:
                    pass
            rows.append(d)
    return rows


def main():
    rows = load(sys.argv[1])
    windows = []
    for arg in sys.argv[2:]:
        name, _, span = arg.partition(":")
        a, _, b = span.partition("-")
        windows.append((name, float(a), float(b)))
    sums = []
    for name, a, b in windows:
        sel = [r for r in rows if a <= r.get("t", -1) <= b]
        tot = {}
        for r in sel:
            for k, v in r.items():
                tot[k] = tot.get(k, 0.0) + v
        sums.append((name, len(sel), tot))
    keys = set()
    for _, _, tot in sums:
        keys.update(k for k in tot if k.endswith("Ns"))
    header = f"{'':28s}" + "".join(f"{name:>22s}" for name, _, _ in sums)
    print(header)

    def line(label, fn):
        print(f"{label:28s}" + "".join(f"{fn(n, tot):>22s}" for _, n, tot in sums))

    line("seconds", lambda n, t: f"{n}")
    line("flips/s", lambda n, t: f"{t.get('FlipsRequested', 0) / max(n, 1):.1f}")
    line("draws/frame", lambda n, t: f"{t.get('Draws', 0) / max(t.get('FlipsRequested', 1), 1):.0f}")
    line("dispatches/frame", lambda n, t: f"{t.get('Dispatches', 0) / max(t.get('FlipsRequested', 1), 1):.0f}")
    for extra in ("ImageCreates", "TextureUploadBytes", "GpuFaults", "SideReadbacks", "ProgramLookups",
                  "MaterializeMisses", "RenderPassBegins", "PipelineCreates"):
        line(extra + "/frame", lambda n, t, e=extra: f"{t.get(e, 0) / max(t.get('FlipsRequested', 1), 1):.0f}")

    order = sorted(keys, key=lambda k: -max(t.get(k, 0) / max(t.get("FlipsRequested", 1), 1) for _, _, t in sums))
    print("--- ms per frame (us per draw) ---")
    for k in order[:45]:
        def cell(n, t, k=k):
            fl = max(t.get("FlipsRequested", 1), 1)
            dr = max(t.get("Draws", 1), 1)
            return f"{t.get(k, 0) / fl / 1e6:7.2f} ({t.get(k, 0) / dr / 1e3:5.2f})"
        line(k, cell)


if __name__ == "__main__":
    main()
