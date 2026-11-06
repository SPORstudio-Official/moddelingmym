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
import tempfile
import subprocess
import sys
import time
import urllib.error
import urllib.request
import wave

API = "https://openrouter.ai/api/v1"

# ---------------------------------------------------------------------------
# The tapes, as dialogue. Each line is (speaker, text) so a scene can be played
# by more than one voice instead of one narrator reading everybody.
# ---------------------------------------------------------------------------

SCRIPT = {
    "tape1": [
        ("operator", "Nine one one. What is your emergency?"),
        ("caller", "I'm off the trail. I don't know where. My radio is dying."),
        ("caller", "There is a sound out here. It's like the air is breathing."),
        ("caller", "It copies me. When I stop, it keeps going."),
        ("caller", "Please. Don't come looking for me. And don't listen to this tape."),
    ],
    "tape2": [
        ("boy", "The recorder is running, Father."),
        ("priest", "Name it. Command the vessel to speak its name."),
        ("boy", "It won't give a name. It gives a number. Sixty three."),
        ("boy", "It lives in the wiring. It sings in the walls."),
        ("priest", "Then we sing louder, boy. We drown it out. Everything on. "
                   "Every light, every fan."),
        ("priest", "And if it learns the room, burn the tape."),
    ],
    "tape3": [
        ("officer", "Sonar reports no contact. Repeat, no contact."),
        ("sailor", "Then who is knocking on the hull."),
        ("officer", "Depth steady. One hundred twenty meters. The water is inside of us."),
        ("sailor", "It tapped back. Three times. That's my brother's code. "
                   "He has been dead six years."),
        ("officer", "Flood the compartment. Flood it. If it can hear us, "
                    "it can follow us up."),
    ],
    "tape4": [
        ("interrogator", "State your name for the record."),
        ("archivist", "You already know my name. You played it back."),
        ("interrogator", "Where were you on the night of the sixth."),
        ("archivist", "In my apartment. Listening to a tape that hasn't been recorded yet."),
        ("interrogator", "What did you do to it. The thing in the wall."),
        ("archivist", "I turned on everything I own. And it still heard me breathing."),
    ],
    "tape5": [
        ("archivist", "This is the room. This is you, ten seconds ago."),
        ("archivist", "Do not look behind the desk."),
        ("archivist", "The microwave will stop at eleven minutes."),
        ("archivist", "I am already inside the machine. Listen."),
    ],
    "entity": [
        ("entity", "Dead air. Dead air."),
        ("entity", "Make a sound. Make a sound."),
        ("entity", "I am the air between the words."),
    ],
}

# One character per speaker. `voice` is passed to OpenRouter ("" = the model's
# own default voice, which is the only other value besides "alloy" this endpoint
# accepts). `shift` is a resample ratio: it moves pitch AND formants together,
# which is what actually changes the apparent person. `eq` adds the room.
SPEAKERS = {
    # 911 dispatcher: dry, procedural, narrow telephone band
    "operator": {"voice": "alloy", "shift": 1.00,
                 "eq": "highpass=f=300,lowpass=f=3400,acompressor=threshold=0.10:ratio=4:attack=4:release=140"},
    # the caller: higher, panicked, close to the mic
    "caller": {"voice": "", "shift": 1.18,
               "eq": "highpass=f=300,lowpass=f=3200,acompressor=threshold=0.07:ratio=6:attack=2:release=90"},
    # the priest: low, slow, churchy tail
    "priest": {"voice": "alloy", "shift": 0.90,
               "eq": "highpass=f=80,lowpass=f=7000,aecho=0.7:0.5:45:0.20"},
    # the altar boy: young, thin, standing too close to the recorder
    "boy": {"voice": "", "shift": 1.12,
            "eq": "highpass=f=120,lowpass=f=6500,aecho=0.6:0.4:28:0.12"},
    # submarine commander: clipped radio voice
    "officer": {"voice": "alloy", "shift": 0.95,
                "eq": "highpass=f=380,lowpass=f=3000,acompressor=threshold=0.09:ratio=8:attack=3:release=70"},
    # sonar sailor: whispering, terrified, same radio
    "sailor": {"voice": "", "shift": 1.06,
               "eq": "highpass=f=350,lowpass=f=3100,acompressor=threshold=0.08:ratio=7:attack=2:release=60"},
    # YOU. the archivist: his own voice on tape 4, and the future tape 5
    "archivist": {"voice": "", "shift": 0.944,
                  "eq": "highpass=f=90,lowpass=f=8000,aecho=0.4:0.3:10:0.07"},
    # the interviewer: flat, cold, no echo at all
    "interrogator": {"voice": "alloy", "shift": 0.86,
                     "eq": "highpass=f=95,lowpass=f=7200"},
    # the thing on the line
    "entity": {"voice": "alloy", "shift": 0.74,
               "eq": "highpass=f=55,lowpass=f=3600,aecho=0.8:0.7:130:0.45"},
}

# kept for the probe scripts
TAPES = {name: "\n\n".join(t for _, t in lines) for name, lines in SCRIPT.items()}

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


def try_speech_endpoint(model, voice, text, key, strict=False):
    # This endpoint only accepts response_format mp3 | pcm.
    for m in (model, model.split(":")[0]):
        for fmt in ("mp3", "pcm"):
            if strict and voice:
                voices = (voice,)
            else:
                voices = (voice, None) if voice else (None,)
            for v in voices:
                body = {"model": m, "input": text, "response_format": fmt}
                if v:
                    body["voice"] = v
                st, ct, data = request(API + "/audio/speech", key, body)
                snippet = "" if st == 200 else data[:200].decode("utf-8", "replace").replace("\n", " ")
                log("A speech model=%s voice=%s fmt=%s -> %s %s %s"
                    % (m, v, fmt, st, ct, snippet))
                if st == 200 and len(data) > 2000:
                    return data, ct, fmt
    return None, None, None


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


def ffmpeg_path():
    """System ffmpeg, else the static binary bundled with imageio-ffmpeg."""
    ff = shutil.which("ffmpeg")
    if ff:
        return ff
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception as e:                                     # noqa: BLE001
        log("no ffmpeg (%s)" % e)
        return None


def pcm_to_wav(data, out_wav, sr=44100):
    """Wrap raw signed 16-bit little-endian mono PCM in a RIFF header."""
    if len(data) % 2:
        data = data[:-1]
    out = wave.open(out_wav, "wb")
    out.setnchannels(1)
    out.setsampwidth(2)
    out.setframerate(sr)
    out.writeframes(data)
    out.close()


def to_wav(data, ctype, out_wav, fmt=None, sr=44100):
    if data[:4] == b"RIFF" or "wav" in (ctype or ""):
        open(out_wav, "wb").write(data)
        return
    looks_mp3 = data[:3] == b"ID3" or (len(data) > 1 and data[0] == 0xFF and data[1] & 0xE0 == 0xE0)
    if fmt == "pcm" and not looks_mp3:
        pcm_to_wav(data, out_wav, sr)
        return
    ff = ffmpeg_path()
    if not ff:
        if fmt == "pcm":
            pcm_to_wav(data, out_wav, sr)
            return
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


def colour(path, shift, chain):
    """Re-voice a take: resample pitch shift (moves formants too - this is what
    makes it read as a different person) with the duration put back by atempo,
    then the speaker's room/EQ chain."""
    ff = ffmpeg_path()
    if not ff:
        return path
    tmp = path + ".col.wav"
    filt = "asetrate=%d,aresample=44100,atempo=%.6f" % (int(44100 * shift), 1.0 / shift)
    if chain:
        filt += "," + chain
    subprocess.check_call([ff, "-y", "-loglevel", "error", "-i", path,
                           "-af", filt, "-ar", "44100", "-ac", "1", tmp])
    os.replace(tmp, path)
    return path


def read_mono(path):
    with wave.open(path) as w:
        sr, ch = w.getframerate(), w.getnchannels()
        a = array.array("h")
        a.frombytes(w.readframes(w.getnframes()))
    if ch > 1:
        a = array.array("h", [sum(a[i:i + ch]) // ch for i in range(0, len(a), ch)])
    if sr != 44100 and len(a) > 1:
        n = int(len(a) * 44100 / sr)
        a = array.array("h", [a[min(len(a) - 1, int(i * sr / 44100))] for i in range(n)])
    return a


def assemble(segments, out, gap_same=0.30, gap_diff=0.50):
    """Join speaker turns, with a longer gap when the speaker changes."""
    frames = array.array("h")
    prev = None
    for speaker, path in segments:
        if prev is not None:
            gap = gap_same if speaker == prev else gap_diff
            frames.extend(array.array("h", [0] * int(44100 * gap)))
        frames.extend(read_mono(path))
        prev = speaker
    w = wave.open(out, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(44100)
    w.writeframes(frames.tobytes())
    w.close()


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

    if args.voice and args.voice.strip().lower() in ("default", "auto", "none"):
        args.voice = ""            # fish rejects "default"; omitting the field uses the model voice
    if args.voice:
        log("requested voice=%s" % args.voice)

    only = [s.strip() for s in args.only.split(",") if s.strip()]
    tmpdir = tempfile.mkdtemp(prefix="deadair-")
    cache = {}
    ok = 0
    for name, lines in SCRIPT.items():
        if only and name not in only:
            continue
        segs = []
        failed = False
        for idx, (speaker, text) in enumerate(lines):
            cfg = SPEAKERS[speaker]
            ckey = (speaker, text)
            seg = cache.get(ckey)
            if seg is None:
                data, ctype, fmt = try_speech_endpoint(args.model, cfg["voice"], text, key)
                if data is None:
                    log("FAILED to synthesise %s line %d (%s)" % (name, idx + 1, speaker))
                    failed = True
                    break
                seg = os.path.join(tmpdir, "%s_%02d_%s.wav" % (name, idx, speaker))
                try:
                    to_wav(data, ctype, seg, fmt)
                    colour(seg, cfg["shift"], cfg["eq"])
                except Exception as e:                          # noqa: BLE001
                    log("FAILED to decode %s line %d: %s" % (name, idx + 1, e))
                    failed = True
                    break
                cache[ckey] = seg
                time.sleep(0.4)
            segs.append((speaker, seg))
            log("  %s line %d/%d  %-12s %s" % (name, idx + 1, len(lines), speaker,
                                               text[:44]))
        if failed:
            break
        path = os.path.join(args.out, name + ".wav")
        assemble(segs, path)
        normalize(path)
        with wave.open(path) as w:
            log("OK %s  %.1fs  %.0f KB  (%d lines, %d voices)"
                % (path, w.getnframes() / float(w.getframerate()),
                   os.path.getsize(path) / 1024.0, len(segs),
                   len({sp for sp, _ in segs})))
        ok += 1
    shutil.rmtree(tmpdir, ignore_errors=True)

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
