#!/usr/bin/env python3
"""
Re-record the five tapes with a different TTS model (OpenRouter).

The sandbox this game was built in cannot reach openrouter.ai, so the embedded
tapes were recorded with a local voice. Run this on a machine with internet
access to swap in another voice, e.g.:

    export OPENROUTER_API_KEY=sk-or-...
    python3 tools/regen_voice_openrouter.py \
        --model fish-audio/s2.1-pro-free:free --voice default

It writes assets/audio/tapeN.wav, rebuilds src/audio_data.h, and then you only
need to re-run scripts/build_windows.sh to get a new DEAD_AIR.exe.

Notes
  * The key is read from the environment and never written to disk or into the
    repository.
  * Endpoint: POST https://openrouter.ai/api/v1/audio/speech
  * If your provider returns mp3/ogg instead of wav, install ffmpeg locally; the
    script converts automatically when ffmpeg is on PATH.
"""
import argparse
import array
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import wave

# Each blank line becomes a pause between spoken phrases.
TAPES = {
    "tape1": """Nine one one. What is your emergency?

I'm off the trail. I don't know where. My radio is dying.

There is a sound out here. It's like the air is breathing.

It copies me. When I stop, it keeps going.

Please. Don't come looking for me. And don't listen to this tape.""",
    "tape2": """The recorder is running, Father.

Name it. Command the vessel to speak its name.

It won't give a name. It gives a number. Sixty three.

It lives in the wiring. It sings in the walls.

Then we sing louder, boy. We drown it out. Everything on. Every light, every fan.

And if it learns the room, burn the tape.""",
    "tape3": """Sonar reports no contact. Repeat, no contact.

Then who is knocking on the hull.

Depth steady. One hundred twenty meters. The water is inside of us.

It tapped back. Three times. That's my brother's code. He has been dead six years.

Flood the compartment. Flood it. If it can hear us, it can follow us up.""",
    "tape4": """State your name for the record.

You already know my name. You played it back.

Where were you on the night of the sixth.

In my apartment. Listening to a tape that hasn't been recorded yet.

What did you do to it. The thing in the wall.

I turned on everything I own. And it still heard me breathing.""",
    "tape5": """This is the room. This is you, ten seconds ago.

Do not look behind the desk.

The microwave will stop at eleven minutes.

I am already inside the machine. Listen.""",
    "entity": """Dead air. Dead air.

Make a sound. Make a sound.

I am the air between the words.""",
}


def synth(model, voice, text, key):
    body = json.dumps({
        "model": model,
        "input": text,
        "voice": voice,
        "response_format": "wav",
    }).encode()
    req = urllib.request.Request(
        "https://openrouter.ai/api/v1/audio/speech",
        data=body,
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/json",
            "HTTP-Referer": "https://github.com/SPORstudio-Official/moddelingmym",
            "X-Title": "DEAD AIR",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=180) as r:
        ctype = r.headers.get("Content-Type", "")
        return r.read(), ctype


def to_pcm16_mono_sr(data, ctype, out_wav, sr=44100):
    """Decode provider bytes into a plain 16-bit mono wav at `sr`."""
    if "wav" in ctype or data[:4] == b"RIFF":
        open(out_wav, "wb").write(data)
        return out_wav
    ff = shutil.which("ffmpeg")
    if not ff:
        sys.exit("provider returned %s, and ffmpeg is not installed to convert it" % ctype)
    tmp = out_wav + ".src"
    open(tmp, "wb").write(data)
    subprocess.check_call([ff, "-y", "-loglevel", "error", "-i", tmp,
                           "-ac", "1", "-ar", str(sr), out_wav])
    os.remove(tmp)
    return out_wav


def normalize(path, peak=0.92):
    w = wave.open(path)
    sr, n, ch = w.getframerate(), w.getnframes(), w.getnchannels()
    raw = w.readframes(n)
    w.close()
    a = array.array("h")
    a.frombytes(raw)
    if ch > 1:
        a = array.array("h", [sum(a[i:i + ch]) // ch for i in range(0, len(a), ch)])
    mx = max(1, max(abs(v) for v in a))
    scale = (peak * 32767.0) / mx
    a = array.array("h", [int(v * scale) for v in a])
    out = wave.open(path, "wb")
    out.setnchannels(1)
    out.setsampwidth(2)
    out.setframerate(sr)
    out.writeframes(a.tobytes())
    out.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="fish-audio/s2.1-pro-free:free")
    ap.add_argument("--voice", default="default")
    ap.add_argument("--only", default="", help="comma separated tape names")
    ap.add_argument("--out", default="assets/audio")
    args = ap.parse_args()

    key = os.environ.get("OPENROUTER_API_KEY", "").strip()
    if not key:
        sys.exit("set OPENROUTER_API_KEY first")

    os.makedirs(args.out, exist_ok=True)
    only = [s.strip() for s in args.only.split(",") if s.strip()]
    for name, text in TAPES.items():
        if only and name not in only:
            continue
        print("recording", name, "...")
        data, ctype = synth(args.model, args.voice, text, key)
        if len(data) < 1000:
            sys.exit("empty audio for %s: %s" % (name, data[:200]))
        path = os.path.join(args.out, name + ".wav")
        to_pcm16_mono_sr(data, ctype, path)
        normalize(path)
        with wave.open(path) as w:
            print("   %s  %.1fs" % (path, w.getnframes() / w.getframerate()))

    print("rebuilding src/audio_data.h ...")
    subprocess.check_call([sys.executable, "tools/gen_audio.py", args.out, "src/audio_data.h"])
    print("done. now run scripts/build_windows.sh")


if __name__ == "__main__":
    main()
