"""
Resting respiration study on SPRO production recordings (read-only).

Breathing rate in each 60 s window of genuine rest, three independent ways:
  rsa  respiratory sinus arrhythmia: the beat-to-beat (RR) interval series
  edr  ECG-derived respiration: the QRS amplitude at each beat
  imu  chest movement: accelerometer, first principal component of the 3 axes
Beats are the human-confirmed beats of the equine-ecg-detector label DB (opened immutable).

Rest = local HR < 60 bpm and std |accel| < 0.05 g in every 10 s of the window (and >= 0.0015 g,
above the stationary-device artefact). Each method's rate is the spectral peak in
0.08-0.6 Hz (4.8-36 breaths/min) of a 60 s Hann window; "purity" = peak band power / total.

    python -B tools/resp_study.py "<...>/Equine ECG R-R Detection" out_dir

Label timeline note: the detector's sample_idx is on the ECG trimmed to the IMU start, so the
position in the ECG file is sample_idx + round((imu_ms - ecg_ms) * 256 / 1000).
"""
import csv
import re
import sqlite3
import sys
from pathlib import Path

import numpy as np
from scipy import signal

FS_ECG = 256.0
FS_R = 4.0             # resampled series (RSA, EDR)
DEC = 52               # IMU 416 Hz -> 8 Hz block means
BAND = (0.08, 0.6)     # Hz
WIN_S, STEP_S = 60.0, 30.0
REST_BPM = 60.0
MOTION_MAX_G, MOTION_MIN_G = 0.05, 0.0015
PAUSE = 1.6            # RR > 1.6 x local median = a dropped beat (2nd-degree AV block) or a gap


def edf_open(path):
    f = open(path, "rb")
    h = f.read(256)
    ns, nrec, dur = int(h[252:256]), int(h[236:244]), float(h[244:252])
    sh = f.read(256 * ns)
    fld = lambda off, w, i: sh[ns * off + i * w: ns * off + (i + 1) * w].decode("latin1").strip()
    e = dict(f=f, hdr=256 + 256 * ns, nrec=nrec, dur=dur,
             labels=[fld(0, 16, i) for i in range(ns)], spr=[int(fld(216, 8, i)) for i in range(ns)],
             pmin=[float(fld(104, 8, i)) for i in range(ns)], pmax=[float(fld(112, 8, i)) for i in range(ns)],
             dmin=[float(fld(120, 8, i)) for i in range(ns)], dmax=[float(fld(128, 8, i)) for i in range(ns)])
    e["recbytes"] = 2 * sum(e["spr"])
    return e


def edf_all(e, labels):
    """Whole-file physical data for `labels` (one read)."""
    e["f"].seek(e["hdr"])
    raw = np.frombuffer(e["f"].read(e["nrec"] * e["recbytes"]), "<i2")
    raw = raw[: (raw.size // (e["recbytes"] // 2)) * (e["recbytes"] // 2)].reshape(-1, e["recbytes"] // 2)
    out = []
    for lab in labels:
        i = e["labels"].index(lab)
        off = sum(e["spr"][:i])
        d = raw[:, off:off + e["spr"][i]].astype(np.float64).ravel()
        g = (e["pmax"][i] - e["pmin"][i]) / (e["dmax"][i] - e["dmin"][i])
        out.append((d - e["dmin"][i]) * g + e["pmin"][i])
    return out


def bandpass(x, fs):
    sos = signal.butter(2, BAND, btype="band", fs=fs, output="sos")
    return signal.sosfiltfilt(sos, x)


def peak_rate(x, fs):
    """Breaths/min at the spectral peak in BAND, and the peak's share of band power."""
    x = (x - x.mean()) * np.hanning(x.size)
    n = 1 << int(np.ceil(np.log2(x.size * 8)))
    p = np.abs(np.fft.rfft(x, n)) ** 2
    f = np.fft.rfftfreq(n, 1 / fs)
    band = (f >= BAND[0]) & (f <= BAND[1])
    if not band.any() or p[band].sum() == 0:
        return np.nan, 0.0
    fb, pb = f[band], p[band]
    k = int(np.argmax(pb))
    near = np.abs(fb - fb[k]) <= 0.025          # +-1.5 breaths/min around the peak
    return 60.0 * fb[k], float(pb[near].sum() / pb.sum())


def main(root, out):
    root, out = Path(root), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(f"file:{root / 'webapp' / 'data' / 'ecg.db'}?mode=ro&immutable=1", uri=True)
    rows = []
    examples = {}
    for sid, fs in db.execute("select id, fs from sessions where n_samples is not null").fetchall():
        folder = root / "test-data" / sid
        files = {p.name.split("_")[0]: p for p in folder.glob("*.edf")} if folder.is_dir() else {}
        if "ecg" not in files or "imu" not in files:
            continue
        ms = {k: int(re.search(r"_(\d+)\.edf$", p.name).group(1)) for k, p in files.items()}
        off_s = (ms["imu"] - ms["ecg"]) / 1000.0
        shift = max(0, int(round((ms["imu"] - ms["ecg"]) * FS_ECG / 1000)))
        idx = np.array([r[0] for r in db.execute(
            "select sample_idx from beats where session_id=? and confirmed=1 order by sample_idx", (sid,))]) + shift
        if idx.size < 60:
            continue
        tb = idx / FS_ECG                                       # beat times, ECG-file seconds
        rr = np.diff(tb)
        loc = signal.medfilt(rr, 11)
        hr_loc = 60.0 / loc
        if not np.any(hr_loc < REST_BPM):
            continue

        e = edf_open(files["ecg"])
        (ecg,) = edf_all(e, ["ECG"])
        e["f"].close()
        hp = ecg - signal.medfilt(ecg, 51)                      # remove baseline wander (~0.2 s)
        amp = np.array([hp[max(0, i - 3): i + 4][np.argmax(np.abs(hp[max(0, i - 3): i + 4]))]
                        if i < hp.size else np.nan for i in idx.astype(int)])

        e = edf_open(files["imu"])
        ax, ay, az = edf_all(e, ["AX", "AY", "AZ"])
        e["f"].close()
        fs_imu = e["spr"][e["labels"].index("AX")] / e["dur"]
        mag = np.sqrt(ax ** 2 + ay ** 2 + az ** 2)
        n8 = ax.size // DEC
        acc8 = np.stack([v[: n8 * DEC].reshape(n8, DEC).mean(1) for v in (ax, ay, az)])
        t8 = off_s + (np.arange(n8) + 0.5) * DEC / fs_imu      # ECG-file seconds
        acc8f = np.stack([bandpass(a, fs_imu / DEC) for a in acc8])

        # RSA and EDR series: beat-pair midpoints, pauses (dropped beats / label gaps) removed
        mid = tb[1:] - rr / 2
        ok = rr < PAUSE * loc
        grid = np.arange(tb[0], tb[-1], 1 / FS_R)
        rsa = np.interp(grid, mid[ok], rr[ok])
        good_amp = np.isfinite(amp)
        edr = np.interp(grid, tb[good_amp], amp[good_amp])
        rsa_f, edr_f = bandpass(rsa, FS_R), bandpass(edr, FS_R)

        w0 = tb[0]
        while w0 + WIN_S <= tb[-1]:
            w1 = w0 + WIN_S
            inb = (tb >= w0) & (tb < w1)
            rr_w = np.diff(tb[inb])
            if rr_w.size < 10:
                w0 += STEP_S
                continue
            hr = 60.0 / np.median(rr_w)
            # motion: every 10 s piece at rest, above the artefact level
            m_ok, mstd = True, []
            for s in np.arange(w0, w1, 10.0):
                i0, i1 = int((s - off_s) * fs_imu), int((s + 10 - off_s) * fs_imu)
                if i0 < 0 or i1 > mag.size:
                    m_ok = False
                    break
                mstd.append(float(np.std(mag[i0:i1])))
            gaps = np.sum(rr_w > 2.5 * np.median(rr_w))              # unlabelled stretches (a dropped
                                                                     # beat is ~2x, kept as a pause)
            if not m_ok or hr >= REST_BPM or max(mstd) >= MOTION_MAX_G or min(mstd) < MOTION_MIN_G or gaps:
                w0 += STEP_S
                continue
            pauses = int(np.sum(rr_w > PAUSE * np.median(rr_w)))
            g = (grid >= w0) & (grid < w1)
            r_rsa, q_rsa = peak_rate(rsa_f[g], FS_R)
            r_edr, q_edr = peak_rate(edr_f[g], FS_R)
            s8 = (t8 >= w0) & (t8 < w1)
            seg = acc8f[:, s8]
            u, sv, vt = np.linalg.svd(seg - seg.mean(1, keepdims=True), full_matrices=False)
            pc = sv[0] * vt[0]
            r_imu, q_imu = peak_rate(pc, fs_imu / DEC)
            rows.append(dict(session=sid[:8], t0=round(w0, 1), hr=round(hr, 1), motion_g=round(np.median(mstd), 4),
                             pauses=pauses, rsa=round(r_rsa, 2), rsa_q=round(q_rsa, 3), edr=round(r_edr, 2),
                             edr_q=round(q_edr, 3), imu=round(r_imu, 2), imu_q=round(q_imu, 3)))
            examples[(sid[:8], round(w0, 1))] = (grid[g] - w0, rsa_f[g], edr_f[g], t8[s8] - w0, pc,
                                                 tb[inb] - w0, rr_w)
            w0 += STEP_S
        print(f"{sid[:8]}: {sum(1 for r in rows if r['session'] == sid[:8])} rest windows", flush=True)

    with open(out / "resp_windows.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    import pickle
    with open(out / "resp_examples.pkl", "wb") as f:
        pickle.dump(examples, f)
    report(rows)


def report(rows):
    r = {k: np.array([x[k] for x in rows], float) for k in rows[0] if k != "session"}
    n = len(rows)
    print(f"\n{n} rest windows (60 s, 50 % overlap) in {len({x['session'] for x in rows})} sessions")
    print(f"HR median {np.median(r['hr']):.1f} bpm; motion median {np.median(r['motion_g']):.4f} g; "
          f"windows with a dropped beat / pause: {np.mean(r['pauses'] > 0):.0%}")
    for k in ("rsa", "edr", "imu"):
        print(f"{k}: median {np.median(r[k]):5.1f} /min, IQR {np.percentile(r[k], 25):.1f}-{np.percentile(r[k], 75):.1f}, "
              f"purity median {np.median(r[k + '_q']):.2f}")
    print("\nagreement (share of windows within +-1.5 / +-3 breaths/min; median |diff|)")
    for a, b in (("imu", "rsa"), ("imu", "edr"), ("rsa", "edr")):
        d = np.abs(r[a] - r[b])
        print(f"  {a} vs {b}: {np.mean(d <= 1.5):5.0%} / {np.mean(d <= 3):5.0%}   median {np.median(d):.1f}")
    bpb = r["hr"] / r["imu"]                 # beats per breath, using the IMU rate
    print(f"\nbeats per breath (HR / IMU rate): median {np.median(bpb):.1f}; "
          f"windows below 2 (RSA under-sampled, Nyquist): {np.mean(bpb < 2):.0%}")
    for lo, hi in ((0, 2), (2, 3), (3, 4), (4, 99)):
        m = (bpb >= lo) & (bpb < hi)
        if m.any():
            d = np.abs(r["imu"][m] - r["rsa"][m])
            print(f"  {lo}-{hi if hi < 99 else '+'} beats/breath: {m.sum():4d} windows, imu~rsa within 1.5: {np.mean(d <= 1.5):.0%}")
    q = r["imu_q"] >= np.percentile(r["imu_q"], 50)
    d = np.abs(r["imu"][q] - r["rsa"][q])
    print(f"\nclearer half of IMU windows (purity >= median): imu~rsa within 1.5: {np.mean(d <= 1.5):.0%}, "
          f"within 3: {np.mean(d <= 3):.0%}")
    for p in (0, 1):
        m = (r["pauses"] > 0) == bool(p)
        d = np.abs(r["imu"][m] - r["rsa"][m])
        print(f"  {'with' if p else 'without'} pauses: {m.sum():4d} windows, imu~rsa within 1.5: {np.mean(d <= 1.5):.0%}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
