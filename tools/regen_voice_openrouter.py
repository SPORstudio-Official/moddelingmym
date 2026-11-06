#!/usr/bin/env python3
"""
Re-record the five tapes with an OpenRouter TTS model.

    export OPENROUTER_API_KEY=sk-or-...
    python3 tools/regen_voice_openrouter.py --model fish-audio/s2.1-pro-free:free --voice default

Writes assets/audio/*.wav, regenerates src/audio_data.h, and always leaves a
diagnostic log in assets/audio/_voice_status.txt (so a CI run can be inspected
without access to the job logs).

Transport order per clip:
  A. POST /api/v1/audio/speech        (OpenAI-compatible speech endpoint)
  B. POST /api/v1/chat/completions    (multimodal: audio comes back base64)
Every attempt is retried without the `voice` field, and without a trailing
":free" qualifier, before giving up.
"""
import argparse
import array
import base64
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import wave

API = "https://openrouter.ai/api/v1"

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

LOG = []


def log(msg):
    line = "%s  %s" % (time.strftime("%H:%M:%S"), msg)
    print(line)
    LOG.append(line)


def request(url, key, payload=None):
    data = None
    headers = {
        "Authorization": "Bearer " + key,
        "Content-Type": "application/json",
        "HTTP-Referer": "https://github.com/SPORstudio-Official/moddelingmym",
        "X-Title": "DEAD AIR",
    }
    if payload is not None:
        data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=240) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", "") if e.headers else "", e.read()
    except Exception as e:                                    # noqa: BLE001
        return 0, "", str(e).encode()


def dump_models(key):
    st, _, body = request(API + "/models", key)
    try:
        models = json.loads(body).get("data", [])
    except Exception:                                          # noqa: BLE001
        log("models: unparseable response (%s)" % st)
        return
    hits = [m.get("id", "") for m in models if "fish" in m.get("id", "").lower()
            or "audio" in json.dumps(m.get("architecture", {})).lower()]
    log("models: %d total, %d audio/fish candidates: %s"
        % (len(models), len(hits), ", ".join(hits[:25]) or "-"))


def try_speech_endpoint(model, voice, text, key):
    for m in (model, model.split(":")[0]):
        for v in (voice, None):
            body = {"model": m, "input": text, "response_format": "wav"}
            if v:
                body["voice"] = v
            st, ct, data = request(API + "/audio/speech", key, body)
            snippet = "" if st == 200 else data[:220].decode("utf-8", "replace").replace("\n", " ")
            log("A speech model=%s voice=%s -> %s %s %s" % (m, v, st, ct, snippet))
            if st == 200 and len(data) > 2000:
                return data, ct
    return None, None


def try_chat_endpoint(model, voice, text, key):
    for m in (model, model.split(":")[0]):
        body = {
            "model": m,
            "modalities": ["text", "audio"],
            "audio": {"voice": voice or "alloy", "format": "wav"},
            "messages": [{"role": "user", "content": text}],
        }
        st, ct, data = request(API + "/chat/completions", key, body)
        if st != 200:
            log("B chat model=%s -> %s %s" % (m, st,
                data[:220].decode("utf-8", "replace").replace("\n", " ")))
            continue
        try:
            js = json.loads(data)
            audio = js["choices"][0]["message"].get("audio") or {}
            b64 = audio.get("data", "")
            if b64:
                if b64.startswith("data:"):
                    b64 = b64.split(",", 1)[1]
                raw = base64.b64decode(b64)
                log("B chat model=%s -> 200 %d bytes (%s)" % (m, len(raw), audio.get("format")))
                return raw, "audio/wav"
            log("B chat model=%s -> 200 but no audio field: %s" % (m, json.dumps(js)[:200]))
        except Exception as e:                                 # noqa: BLE001
            log("B chat model=%s -> parse error %s" % (m, e))
    return None, None


def to_wav(data, ctype, out_wav, sr=44100):
    if data[:4] == b"RIFF" or "wav" in (ctype or ""):
        open(out_wav, "wb").write(data)
        return
    ff = shutil.which("ffmpeg")
    if not ff:
        raise RuntimeError("no wav returned (%s) and ffmpeg unavailable" % ctype)
    tmp = out_wav + ".src"
    open(tmp, "wb").write(data)
    subprocess.check_call([ff, "-y", "-loglevel", "error", "-i", tmp,
                           "-ac", "1", "-ar", str(sr), out_wav])
    os.remove(tmp)


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
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default="assets/audio")
    args = ap.parse_args()

    key = ""
    key_src = ""
    for name in ("OPENROUTER_API_KEY", "OPENROUTER_KEY", "OPENROUTER_TOKEN",
                 "OR_API_KEY", "OPENROUTER_API", "FISH_API_KEY", "OPENROUTER",
                 "OPENROUTERAPIKEY", "OPEN_ROUTER_KEY", "API_KEY", "SK_OR_KEY",
                 "VAR_OPENROUTER_API_KEY", "VAR_OPENROUTER_KEY", "DISPATCH_KEY"):
        v = os.environ.get(name, "").strip()
        if v:
            key, key_src = v, v if False else name
            break
    os.makedirs(args.out, exist_ok=True)
    status_path = os.path.join(args.out, "_voice_status.txt")

    def finish(code):
        open(status_path, "w").write("\n".join(LOG) + "\n")
        sys.exit(code)

    present = [n for n in ("OPENROUTER_API_KEY", "OPENROUTER_KEY", "OPENROUTER_TOKEN",
                           "OR_API_KEY", "OPENROUTER_API", "FISH_API_KEY", "OPENROUTER",
                           "OPENROUTERAPIKEY", "OPEN_ROUTER_KEY", "API_KEY", "SK_OR_KEY",
                           "VAR_OPENROUTER_API_KEY", "VAR_OPENROUTER_KEY", "DISPATCH_KEY")
               if os.environ.get(n, "").strip()]
    log("env vars with a value: %s" % (", ".join(present) or "NONE"))
    if not key:
        log("no API key found -> cannot record")
        finish(1)
    log("using %s (len %d); model=%s voice=%s"
        % (key_src, len(key), args.model, args.voice))
    if key_src.startswith("VAR_"):
        log("WARNING: the key came from a plain repository *variable*, i.e. it is "
            "readable by anyone. Move it to a secret and rotate it.")

    dump_models(key)

    only = [s.strip() for s in args.only.split(",") if s.strip()]
    ok = 0
    for name, text in TAPES.items():
        if only and name not in only:
            continue
        data, ctype = try_speech_endpoint(args.model, args.voice, text, key)
        if data is None:
            data, ctype = try_chat_endpoint(args.model, args.voice, text, key)
        if data is None:
            log("FAILED to synthesise %s" % name)
            break
        path = os.path.join(args.out, name + ".wav")
        try:
            to_wav(data, ctype, path)
            normalize(path)
        except Exception as e:                                 # noqa: BLE001
            log("FAILED to decode %s: %s" % (name, e))
            break
        with wave.open(path) as w:
            log("OK %s  %.1fs  %.0f KB" % (path, w.getnframes() / w.getframerate(),
                                           os.path.getsize(path) / 1024.0))
        ok += 1

    if ok == 0:
        finish(1)
    if ok < len(TAPES) and not only:
        log("only %d/%d clips recorded" % (ok, len(TAPES)))
        finish(1)

    log("rebuilding src/audio_data.h")
    subprocess.check_call([sys.executable, "tools/gen_audio.py", args.out, "src/audio_data.h"])
    log("DONE")
    finish(0)


if __name__ == "__main__":
    main()
