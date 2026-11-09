"""Check a VPro EDF recording (header, record count vs file size, signal stats).

    python tools/edf_info.py E:/1794184211/ECG.edf

Pure Python (no dependencies). For viewing, open the file in EDFbrowser.
"""
import os
import statistics
import struct
import sys


def field(h, off, n):
    return h[off:off + n].decode("ascii", "replace").strip()


def main(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        h = f.read(256)
        ns = int(field(h, 252, 4))
        sig = f.read(256 * ns)
        hdr_bytes = int(field(h, 184, 8))
        nrec = int(field(h, 236, 8))
        dur = float(field(h, 244, 8))
        print(f"file        {path} ({size} bytes)")
        print(f"patient     {field(h, 8, 80)}")
        print(f"recording   {field(h, 88, 80)}")
        print(f"start       {field(h, 168, 8)} {field(h, 176, 8)}")
        print(f"records     {nrec} x {dur:g} s, {ns} signal(s), header {hdr_bytes} bytes")

        def sfield(i, off, n):
            return sig[off * ns + i * n: off * ns + (i + 1) * n].decode("ascii", "replace").strip()

        spr, scale = [], []
        for i in range(ns):
            label = sfield(i, 0, 16)
            unit = sfield(i, 96, 8)
            pmin, pmax = float(sfield(i, 104, 8)), float(sfield(i, 112, 8))
            dmin, dmax = int(sfield(i, 120, 8)), int(sfield(i, 128, 8))
            n = int(sfield(i, 216, 8))
            spr.append(n)
            k = (pmax - pmin) / (dmax - dmin)
            scale.append((k, pmin - dmin * k))
            print(f"signal {i}    {label}: {n / dur:g} Hz, {unit}, {k:.6f} {unit}/LSB, prefilter {sfield(i, 136, 80)}")

        rec_bytes = 2 * sum(spr)
        actual = (size - hdr_bytes) // rec_bytes
        extra = (size - hdr_bytes) % rec_bytes
        print(f"data        {actual} complete records in the file"
              + (f" + {extra} stray bytes" if extra else "")
              + ("" if actual == nrec else f"  (header says {nrec})"))

        # Per-minute stats of signal 0
        per_min = int(60 / dur)
        f.seek(hdr_bytes)
        for m in range(0, actual, per_min):
            vals = []
            for _ in range(min(per_min, actual - m)):
                rec = f.read(rec_bytes)
                vals.extend(struct.unpack(f"<{spr[0]}h", rec[:2 * spr[0]]))
            k, off = scale[0]
            phys = [v * k + off for v in vals]
            flat = sum(1 for a, b in zip(vals, vals[1:]) if a == b) / max(1, len(vals) - 1)
            print(f"  min {m // per_min:3d}: mean {statistics.fmean(phys):8.1f}  sd {statistics.pstdev(phys):7.1f}"
                  f"  range {min(phys):8.1f} .. {max(phys):8.1f}  repeated {flat:5.1%}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
