#!/usr/bin/env python3
"""Probe how stable the OpenRouter TTS voice is.

Synthesises one line twice (same model, same text, no voice reference) plus a
second, different line, then measures how similar the takes are. Answers two
questions:

  * is the "default" voice a *solid* speaker, or a fresh random voice per call?
  * does the speaker stay the same when the words change?

Writes demo/probe_*.wav and a text report at assets/audio/_voice_probe.txt.
Needs: numpy, imageio-ffmpeg (for mp3 -> wav).
"""

import importlib.util
import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "rv", os.path.join(HERE, "regen_voice_openrouter.py"))
rv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rv)

MODEL = os.environ.get("PROBE_MODEL") or "fish-audio/s2.1-pro-free:free"
LINE_A = "Reel one. Channel two. This is the archive, and nobody is listening."
LINE_B = "The room is empty now, and the microphone is still running."

REPORT = "assets/audio/_voice_probe.txt"
OUT = "demo"


def find_key():
    for name in ("DISPATCH_KEY", "OPENROUTER_API_KEY", "OPENROUTER_KEY",
                 "OPENROUTER_TOKEN", "OR_API_KEY", "OPENROUTER_API", "FISH_API_KEY",
                 "OPENROUTER", "OPENROUTERAPIKEY", "OPEN_ROUTER_KEY", "API_KEY",
                 "SK_OR_KEY", "VAR_OPENROUTER_API_KEY", "VAR_OPENROUTER_KEY"):
        v = os.environ.get(name, "").strip()
        if v:
            return v, name
    return "", ""


def load(path):
    w = wave.open(path)
    sr, ch = w.getframerate(), w.getnchannels()
    a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32) / 32768.0
    w.close()
    if ch > 1:
        a = a.reshape(-1, ch).mean(axis=1)
    return a, sr


def voiceprint(path, nbands=24):
    """Mean log-spectrum in log-spaced bands over voiced frames (timbre fingerprint)."""
    a, sr = load(path)
    edges = np.geomspace(120, 7000, nbands + 1)
    f = np.fft.rfftfreq(2048, 1.0 / sr)
    spec = np.zeros(len(f))
    cnt = 0
    for i in range(0, len(a) - 2048, 1024):
        fr = a[i:i + 2048] * np.hanning(2048)
        if np.sqrt((fr ** 2).mean()) < 0.03:
            continue
        spec += np.abs(np.fft.rfft(fr)) ** 2
        cnt += 1
    spec /= max(cnt, 1)
    bands = np.array([spec[(f >= edges[k]) & (f < edges[k + 1])].mean()
                      for k in range(nbands)])
    v = np.log(bands + 1e-12)
    return (v - v.mean()) / (v.std() + 1e-9)


def median_f0(path, fmin=70, fmax=400, thr=0.30):
    """YIN pitch tracker -> median F0 of clearly voiced frames."""
    a, sr = load(path)
    out = []
    win = int(sr * 0.06)
    for i in range(0, len(a) - win, int(sr * 0.02)):
        fr = a[i:i + win]
        if np.sqrt((fr ** 2).mean()) < 0.035:
            continue
        tau_max = min(int(sr / fmin), win // 2)
        d = np.array([np.dot(fr[:win - t] - fr[t:], fr[:win - t] - fr[t:])
                      for t in range(1, tau_max + 1)])
        cm = np.empty_like(d)
        run = 0.0
        for t in range(1, tau_max + 1):
            run += d[t - 1]
            cm[t - 1] = d[t - 1] * t / run if run > 0 else 1.0
        hit = None
        for t in range(int(sr / fmax), tau_max):
            if cm[t] < thr:
                while t + 1 < len(cm) and cm[t + 1] < cm[t]:
                    t += 1
                hit = sr / (t + 1)
                break
        if hit:
            out.append(hit)
    return float(np.median(out)) if out else 0.0


def synth(text, key, tag):
    import subprocess
    os.makedirs(OUT, exist_ok=True)
    data, ctype, fmt = rv.try_speech_endpoint(MODEL, "", text, key)
    if data is None:
        rv.log("probe: synthesis failed for %s" % tag)
        return None
    path = os.path.join(OUT, "probe_%s.wav" % tag)
    rv.to_wav(data, ctype, path, fmt)
    rv.normalize(path)
    with wave.open(path) as w:
        rv.log("probe: %s -> %.1fs" % (path, w.getnframes() / float(w.getframerate())))
    return path


def main():
    key, src = find_key()
    if not key:
        rv.log("probe: no API key in this run")
        return 1
    rv.log("probe: key from %s; model=%s" % (src, MODEL))

    a1 = synth(LINE_A, key, "same_text_1")
    a2 = synth(LINE_A, key, "same_text_2")
    b1 = synth(LINE_B, key, "other_text")
    if not (a1 and a2 and b1):
        return 1

    v1, v2, vb = voiceprint(a1), voiceprint(a2), voiceprint(b1)
    same = float(np.corrcoef(v1, v2)[0, 1])
    other = float(np.corrcoef(v1, vb)[0, 1])
    f1, f2, fb = median_f0(a1), median_f0(a2), median_f0(b1)

    lines = [
        "voice probe  model=%s  voice=<omitted>" % MODEL,
        "",
        "two takes of the same line : timbre r=%.3f   median F0 %.0f Hz vs %.0f Hz"
        % (same, f1, f2),
        "one take of a different line: timbre r=%.3f   median F0 %.0f Hz"
        % (other, fb),
        "",
        "r is the correlation of the two spectral envelopes (1.0 = identical timbre).",
    ]
    if same > 0.97:
        lines.append("VERDICT: the default voice is SOLID - same speaker on every call.")
    elif same > 0.90:
        lines.append("VERDICT: same speaker, with normal take-to-take variation "
                     "(delivery/timing differs, timbre matches).")
    else:
        lines.append("VERDICT: the default voice is NOT stable - each call is a new "
                     "voice, so the tapes' narrator would change per tape.")

    with open(REPORT, "w") as f:
        f.write("\n".join(lines) + "\n")
    rv.log("probe report -> %s" % REPORT)
    for line in lines:
        rv.log("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
