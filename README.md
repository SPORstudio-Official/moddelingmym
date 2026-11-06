# DEAD AIR

**A tactile / auditory psychological horror game for Windows.**
It is 1996. You are a freelance audio archivist. You are paid to restore damaged tapes.
You are not being hunted by something you can see. You are being let in, one clean line at a time.

In most horror games you have to stay quiet so the monster doesn't hear you.
In **DEAD AIR** you have to make noise to survive.

---

## Download

**Standalone Windows build (no installer, no runtime, no dependencies):**

https://github.com/SPORstudio-Official/moddelingmym/releases/download/deadair/DEAD_AIR.exe

Checksum: https://github.com/SPORstudio-Official/moddelingmym/releases/download/deadair/DEAD_AIR.exe.sha256

That asset is rebuilt and replaced by `.github/workflows/deadair-build.yml` on every
push that touches the game sources.

* Windows 10 / 11, 64-bit. Download the single `.exe` (~3.8 MB) and run it.
* It is unsigned, so Windows SmartScreen may show *"Windows protected your PC"* → **More info → Run anyway**.
* Headphones are strongly recommended. Sound is the game.

### If your antivirus calls it a threat

It is a **false positive**, and an unsigned, freshly-built 3.9 MB binary is the
classic shape of one. What the executable actually does, in full:

| | |
|---|---|
| Imports | `WINMM.dll`, `GDI32.dll`, `USER32.dll`, the UCRT — nothing else |
| Network | none. No Winsock, no WinHTTP, no URLDownloadToFile |
| Files | none. It never calls `CreateFile`; there is no save file, no config, no temp data |
| Processes | none. No `CreateProcess`, `WinExec`, `ShellExecute`, `LoadLibrary` |
| Registry | none |
| Audio | `waveOut*` only, samples are compiled into the binary |

Version 1.0.1 embeds a proper application manifest (`asInvoker`, DPI-aware,
Win7–Win11 supportedOS), version metadata and `DYNAMICBASE` / `NX_COMPAT` /
`HIGH_ENTROPY_VA` linker flags, which is what most heuristic engines expect to
see. That removes some hits, but only a signature removes the rest — there is no
code-signing certificate on this project yet.

To confirm the file you have is the file CI built:

```powershell
certutil -hashfile DEAD_AIR.exe SHA256
# compare against the published DEAD_AIR.exe.sha256
```

Then, if a scanner still objects, report it as a false positive and it clears for
everyone in a day or two:

* Microsoft Defender / SmartScreen — <https://www.microsoft.com/en-us/wdsi/filesubmission>
* Kaspersky OpenTIP — <https://opentip.kaspersky.com/>
* Avast / AVG — <https://www.avast.com/false-positive-file-form.php>
* Bitdefender — <https://www.bitdefender.com/consumer/support/answer/29358/>

Or skip the binary entirely and build it yourself — it is one command and needs
nothing but a Zig toolchain:

```bash
pip install ziglang && ./scripts/build_windows.sh   # writes build/DEAD_AIR.exe
```

---

## How it plays

You never walk around a level. You sit at one desk in one small apartment:

* a **reel-to-reel deck** with `LOW CUT` and `HIGH CUT` knobs,
* a **CRT archive terminal** that shows the transcript you are restoring,
* an **SPL meter** that is the closest thing this game has to a health bar.

**The job.** Play the tape, tune the two filters until the redacted words become
legible, then fill them in from the word bank (click, or press `1`–`7`).

**The threat.** The clearer the signal, the more of the entity comes through into
your room. Interference climbs while the voice is clean. When the SPL needle sits
in the red too long, it finds you.

**The anti-stealth mechanic.** Every tape is labelled with a frequency (`63HZ`,
`118HZ`, `47HZ`…). Get up from the desk (`SPACE`), walk into the room view and
switch on something that hums at the same frequency — the microwave, the fan, the
shower (broadband), or the tunable radio. Matching hums cancel the entity's band.

**The tension.** Every machine adds load to the mains. Too much load and the fuse
blows: blackout, no tape, no light, and something in the dark. The breaker is by
the front door. You also cannot hear the tape over your own appliances, so every
machine you switch on costs you signal quality.

### Controls

| Input | Action |
|---|---|
| Mouse | Knobs (drag up/down), radio tuning (drag), word bank, appliances, breaker |
| `SPACE` | Stand up / sit down, start, continue |
| `1`–`7` | Pick a word bank entry |
| `ESC` | Pause |
| `F11` | Fullscreen |

### The five nights

1. **TAPE 01** — a 911 call from a missing hiker. Tutorial: tune, transcribe, listen.
2. **TAPE 02** — an exorcism. The mechanic opens up: drown the chanting with your own machines.
3. **TAPE 03** — a submarine's final hour. Water drips from your ceiling, and the entity starts switching your appliances *off* to trick you.
4. **TAPE 04** — a police interview whose subject has your voice, recorded tomorrow.
5. **TAPE 05** — an unlabelled tape recording your apartment ten seconds into the future. Read the live feed and arm the room it names before impact.

All five tapes and the thing on the line are voiced by
**`fish-audio/s2.1-pro-free`** through OpenRouter, recorded by CI and embedded as
raw PCM. Everything else — the rain, the hums, the creaks, the drone, the thing in
the closet — is synthesised at runtime.

---

## Building from source

The whole game is C99, software-rendered at 640×360 and scaled up with nearest
neighbour (the chunky look is intentional). It cross-compiles to a single Windows
executable with [Zig](https://ziglang.org):

```bash
# 1. Windows .exe  (needs zig)
ZIG=/path/to/zig ./scripts/build_windows.sh
# -> build/DEAD_AIR.exe

# 2. Linux/dev harness used to iterate on the game core headlessly (needs gcc)
./scripts/build_harness.sh
./build/harness tools/scripts/desk.txt   # dumps BMP frames
```

No third-party libraries. The Win32 layer uses `waveOut` for audio and a DIB
section + `StretchBlt` for video.

### Layout

```
src/da.h            shared types, colours, audio params, night data structs
src/draw.c          software framebuffer primitives + 6x10 bitmap font renderer
src/font_data.h     generated 1-bit glyph atlas (tools/gen_font.py)
src/audio_data.h    generated 16 kHz PCM of the six recorded tapes (tools/gen_audio.py)
src/dsp.c           procedural audio engine (tape chain, appliances, entity, SFX)
src/content.c       the five tapes: briefing text, transcript lines, difficulty
src/face.c          the thing on the tape (drawn, not modelled)
src/game.c          rules, state machine, every screen
src/main_win32.c    Win32 platform layer (window, blit, waveOut thread)
src/harness.c       headless driver: scripted input -> BMP frames / WAV
tools/              asset generators + test scripts
```

Audio assets were recorded with licensed TTS and embedded as raw PCM; the game
does no file I/O at runtime.

### Changing the voice

The current tapes were recorded with OpenRouter's
`fish-audio/s2.1-pro-free:free` (voice = model default; the endpoint rejects the
literal id `default`, so leave `voice=` empty to get the model's own voice). To
re-record them, run this on a machine with normal internet access:

```bash
export OPENROUTER_API_KEY=sk-or-...
python3 -m pip install numpy imageio-ffmpeg     # imageio-ffmpeg supplies a static ffmpeg
python3 tools/regen_voice_openrouter.py --model fish-audio/s2.1-pro-free:free --voice ""
./scripts/build_windows.sh
```

The script rewrites `assets/audio/*.wav`, regenerates `src/audio_data.h` and
leaves you a fresh `build/DEAD_AIR.exe`. The key is read from the environment and
is never written into the repository.

The same thing runs in CI, which is the only path out of a locked-down environment
(the build sandbox cannot reach openrouter.ai):

1. Add the repository secret `OPENROUTER_API_KEY` (Settings -> Secrets and
   variables -> Actions -> New repository secret).
2. `.github/voice-request.txt` already asks for `fish-audio/s2.1-pro-free:free`;
   edit `model=` / `voice=` there to change it.
3. Trigger the workflow (any push, or *Run workflow* in the Actions tab). To pass
   a key without storing it at all, dispatch with it in the payload:

```bash
gh api -X POST repos/SPORstudio-Official/moddelingmym/actions/workflows/deadair-build.yml/dispatches \
  -f ref=arena/96c135af-moddelingmym \
  -f inputs[openrouter_key]=sk-or-... \
  -f inputs[model]=fish-audio/s2.1-pro-free:free -f inputs[voice]=default
```

   Runs with no key at all are skipped cleanly and keep the embedded tapes.

The job re-records the tapes, commits them back to the branch, rebuilds the exe
and replaces the release asset.
