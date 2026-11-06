#!/usr/bin/env python3
"""Discover which `voice` values the OpenRouter TTS endpoint actually accepts,
and whether prosody markup in the input text changes the delivery.

Writes demo/voice_<name>.wav for every candidate that returns audio, plus a
report at assets/audio/_voice_voices.txt with the status code, duration and
median pitch of each take, and the model metadata block for the fish model.
"""

import importlib.util
import json
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
LINE = "Static, then a voice. Testing, one, two, three."
REPORT = "assets/audio/_voice_voices.txt"
OUT = "demo"

CANDIDATES = [
    "alloy", "ash", "ballad", "coral", "echo", "fable", "nova", "onyx",
    "sage", "shimmer", "verse", "marin", "cedar",
    "male", "female", "man", "woman", "narrator", "default",
    "Sarah", "Emily", "Dylan", "Vivian", "Aria", "Roger", "Laura",
    "en-US-1", "en_us_1", "af_heart", "am_echo",
]

PROSODY_PLAIN = "I opened the door and there was nothing there."
PROSODY_TAGGED = ("[whispering] I opened the door... and there was "
                  "nothing there. [sighs] Nothing at all.")


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


def median_f0(path, fmin=70, fmax=400, thr=0.32):
    a, sr = load(path)
    out = []
    win = int(sr * 0.05)
    for i in range(0, len(a) - win, int(sr * 0.02)):
        fr = a[i:i + win]
        if np.sqrt((fr ** 2).mean()) < 0.04:
            continue
        tau_max = min(int(sr / fmin), win // 2)
        d = np.array([np.dot(fr[:win - t] - fr[t:], fr[:win - t] - fr[t:])
                      for t in range(1, tau_max + 1)])
        cm = np.empty_like(d)
        run = 0.0
        for t in range(1, tau_max + 1):
            run += d[t - 1]
            cm[t - 1] = d[t - 1] * t / run if run > 0 else 1.0
        for t in range(int(sr / fmax), tau_max):
            if cm[t] < thr:
                while t + 1 < len(cm) and cm[t + 1] < cm[t]:
                    t += 1
                out.append(sr / (t + 1))
                break
    return float(np.median(out)) if out else 0.0


def model_meta(key):
    import urllib.request
    req = urllib.request.Request(rv.API + "/models",
                                 headers={"Authorization": "Bearer " + key})
    try:
        with urllib.request.urlopen(req, timeout=45) as r:
            js = json.loads(r.read())
    except Exception as e:                                     # noqa: BLE001
        return "models: fetch failed (%s)" % e
    for m in js.get("data", []):
        if "fish" in m.get("id", ""):
            return "model entry: " + json.dumps(m)[:900]
    return "no fish entry in /models"


def take(key, voice, text, tag):
    data, ctype, fmt = rv.try_speech_endpoint(MODEL, voice, text, key,
                                              strict=bool(voice))
    if data is None:
        return None
    path = os.path.join(OUT, "%s.wav" % tag)
    try:
        rv.to_wav(data, ctype, path, fmt)
        rv.normalize(path)
    except Exception as e:                                     # noqa: BLE001
        rv.log("probe: decode failed for %s: %s" % (tag, e))
        return None
    return path


def main():
    key, src = find_key()
    if not key:
        rv.log("probe: no API key in this run")
        return 1
    os.makedirs(OUT, exist_ok=True)
    rv.log("probe: key from %s; model=%s" % (src, MODEL))

    lines = ["voice discovery  model=%s" % MODEL, "", model_meta(key), ""]
    good = []
    for voice in CANDIDATES:
        path = take(key, voice, LINE, "voice_%s" % voice.replace("/", "_"))
        if path:
            a, sr = load(path)
            f0 = median_f0(path)
            good.append((voice, len(a) / sr, f0))
            lines.append("ACCEPTED  voice=%-12s  %.1fs  median F0 %.0f Hz" % (voice, len(a) / sr, f0))
            rv.log("probe: voice=%s ACCEPTED (%.1fs, %.0f Hz)" % (voice, len(a) / sr, f0))
        else:
            lines.append("rejected  voice=%-12s" % voice)

    lines.append("")
    lines.append("-- prosody markup --")
    p1 = take(key, "", PROSODY_PLAIN, "prosody_plain")
    p2 = take(key, "", PROSODY_TAGGED, "prosody_tagged")
    if p1 and p2:
        a1, _ = load(p1)
        a2, _ = load(p2)
        lines.append("plain  %.2fs   tagged  %.2fs   (tagged should be noticeably "
                     "longer/different if the markup is honoured)"
                     % (len(a1) / 44100.0, len(a2) / 44100.0))
    else:
        lines.append("prosody probe failed")

    lines.append("")
    lines.append("accepted voices: %s" % (", ".join(v for v, _, _ in good) or "NONE"))
    with open(REPORT, "w") as f:
        f.write("\n".join(lines) + "\n")
    for line in lines:
        rv.log("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
