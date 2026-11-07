"""Second-by-second timeline of one perf log (transition analysis).

Usage: python -I timeline.py LOG START END
Columns: flips, draws per frame, PM4 ms, shader compile ms/count, pipeline create ms, dispatch
(op16) ms, draw program lookup ms, GPU wait ms, guest-fault ms, image creates, texture upload GB.
"""
import sys


def main():
    path, start, end = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.replace("\0", "")
            if not line.startswith("t="):
                continue
            d = {}
            for p in line.split():
                k, sep, v = p.partition("=")
                if not sep:
                    continue
                if "/" in v:
                    k, v = k + ".Ns", v.partition("/")[2]
                try:
                    d[k] = float(v)
                except ValueError:
                    pass
            t = d.get("t", -1)
            if t < start or t > end:
                continue

            def g(k, s=1.0):
                return d.get(k, 0.0) / s

            fl = g("FlipsRequested")
            print(f"{t:6.1f} fl={fl:3.0f} dr/f={g('Draws') / max(1, fl):5.0f} pm4={g('Pm4ProcessNs', 1e6):5.0f} "
                  f"sc={g('ShaderCompileNs', 1e6):4.0f}/{g('ShadersCompiled'):3.0f} pc={g('PipelineCreateNs', 1e6):4.0f} "
                  f"op16={g('op16.Ns', 1e6):5.0f} prog={g('DrawProgramsNs', 1e6):4.0f} gw={g('GpuWaitNs', 1e6):4.0f} "
                  f"flt={g('GpuFaultNs', 1e6):4.0f} img={g('ImageCreates'):4.0f} tex={g('TextureUploadBytes', 1e9):4.1f}G")


if __name__ == "__main__":
    main()
