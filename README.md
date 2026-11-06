# Colony — Ant Colony Lab

A self-contained desktop colony simulation: shape the terrain, place food, and watch a colony discover, share, and improve routes.

## Download for Windows

[**Download Colony.exe**](https://github.com/SPORstudio-Official/moddelingmym/releases/latest/download/Colony.exe)

This is a portable 64-bit Windows desktop app. Download the single `.exe` and run it—no browser, Python, installation, account, or internet connection is needed. Colony and its simulation assets are bundled inside the executable.

## Field controls

- **Food (1):** left-click to drop a pile. Right-click a pile to remove it. Click nearby again to add more.
- **Obstacle (2):** click to place a rock that workers need to learn around.
- **Eraser (3):** click a food pile or rock to remove it.
- Pause/resume the simulation and cycle speeds from 1× to 4×.
- **Clear edits** restores the starting field. Refresh the app to reset the colony and its learned policy.

## A smarter colony

- **Scouts** explore fresh ground; **foragers** exploit remembered sources.
- A shared colony memory lets workers direct one another toward discovered food. Foragers rank sources by distance, remaining yield, learned quality, and crowding, then periodically reconsider assignments.
- Returning workers lay **food pheromone**. Stronger, successful routes persist longer and guide other workers; scent fades over time.
- Each worker also uses a shared online, tabular **Q-learning** policy: 64 states combine goal bearing, nearby obstacle layout, and carry intent; eight compass actions are scored. Progress is rewarded, collisions penalized, and successful food collection and delivery reinforce decisions.
- The queen grows the workforce when the nest has enough stored food. Scouts, foragers, food stores, deliveries, and live policy values are visible in the interface.

This is a compact, inspectable reinforcement-learning system, not a pretrained neural model. It runs locally in the app; no user data or simulation state is sent anywhere.

## Run from source

Open `index.html` in a modern browser, or serve this directory:

```bash
python -m http.server 8000
```

Then visit `http://localhost:8000`.

## Build the Windows executable

The GitHub Actions workflow builds `Colony.exe` on Windows for pushes to `main` and the active development branch, then updates the `colony-latest` release. To build on Windows locally:

```powershell
npm ci
npm run dist:win
```

The portable `.exe` is written to `release\Colony.exe`.
