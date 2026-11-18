#!/usr/bin/env python3
"""Click / discontinuity statistics of an audio dump (audio_volcado_salida_s -> audio_salida.wav, float32).

Usage: audio_clicks.py <wav> [<wav> ...]

For each file: length, RMS, peak, clipped samples (|x| >= 1), and "clicks": samples whose second difference
is more than 8x the local (10 ms) RMS of the second difference and above an absolute floor. Misaligned XMA
loops and dropped/repeated blocks show up as clicks; clean music and ambience have few.
"""
import struct
import sys

import numpy as np


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(samples, dtype="<f4")
    elif tag == 1 and bits == 16:
        x = np.frombuffer(samples, dtype="<i2").astype(np.float32) / 32768.0
    else:
        raise ValueError("unsupported WAV format %d/%d" % (tag, bits))
    return x[: len(x) // channels * channels].reshape(-1, channels), rate


def stats(path):
    x, rate = read_wav(path)
    mono = x.mean(axis=1).astype(np.float64)
    d2 = np.abs(np.diff(mono, 2))
    win = max(1, rate // 100)
    kernel = np.ones(win) / win
    local = np.sqrt(np.convolve(d2 * d2, kernel, mode="same")) + 1e-9
    clicks = np.flatnonzero((d2 > 8.0 * local) & (d2 > 0.02))
    # Count clusters (one click event can be several samples).
    events = int(np.count_nonzero(np.diff(clicks) > rate // 1000)) + (1 if clicks.size else 0)
    secs = len(mono) / rate
    silent = float(np.mean(np.abs(mono) < 1e-4))
    print("%s: %.1f s, %d Hz, %d ch, rms %.4f, peak %.3f, clipped %d, silent %.0f%%, clicks %d (%.2f/s)" % (
        path, secs, rate, x.shape[1], float(np.sqrt(np.mean(mono * mono))), float(np.max(np.abs(x))),
        int(np.count_nonzero(np.abs(x) >= 1.0)), silent * 100, events, events / max(secs, 1e-9)))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        stats(p)
