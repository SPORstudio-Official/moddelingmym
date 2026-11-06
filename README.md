# Wonderworks — The Creativity Arcade

Wonderworks is a cozy, playful idea-making game. Each round combines a tiny challenge with three unexpected ingredients: a strange thing, a new point of view, and a playful rule. Turn that recipe into an invention, sketch a prototype, and save the result in your private Idea Vault.

It is designed to make creative practice feel easy and fun—not to grade anyone's talent. Sparks reward trying new combinations, adding a little detail, sketching, and reflecting on how an idea feels. They are not a score of how creative you are.

## Download for Windows

[**Download Wonderworks.exe**](https://github.com/SPORstudio-Official/moddelingmym/releases/download/wonderworks-latest/Wonderworks.exe)

Portable 64-bit desktop app. Download and run the one `.exe`; no installation, account, or internet connection is needed. Windows may show a SmartScreen warning because this small project is not signed with a commercial code-signing certificate.

## Play

1. Pick one seed from each shelf: **The unexpected thing**, **A different point of view**, and **A playful rule**.
2. Give your idea a name and a sentence. Sketching is optional; rough, funny drawings are welcome.
3. Choose how the idea feels, then **Plant this idea** to add it to your Idea Vault.
4. Remix for a fresh brief any time. The Vault can filter, copy, remove, or export your ideas as JSON.

There is no timer and no wrong answer. Try an ingredient you would normally skip, connect two unrelated things, or follow the oddest part of your first thought.

## Your ideas stay yours

The game makes no network requests. Spark count, settings, and up to 60 idea cards are saved locally in app/browser storage. The Vault is private to this device unless you export it. The packaged app does not need an internet connection to play.

## Run from source

```bash
npm ci
npm start
```

The workshop can also run in a current browser by opening `index.html` or serving this directory with a static server. No game server is required.

## Build the Windows app

Requires Node.js 22 or newer and npm:

```bash
npm ci
npm run check
npm run dist:win
```

The x64 portable app is written to `release/Wonderworks.exe`. The GitHub Actions workflow builds and publishes the refreshed standalone download for pushes to `main` and `arena/**`.

## Project structure

- `index.html` — game screens and accessible controls
- `style.css` — responsive visual design and motion
- `game.js` — prompt decks, sketchpad, rewards, save system, and Idea Vault
- `electron/main.cjs` — locked-down desktop window
- `assets/icon.svg`, `assets/icon.png`, `assets/icon.ico` — app identity and platform icons
