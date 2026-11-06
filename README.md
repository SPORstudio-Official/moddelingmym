# Colony — Ant Colony Lab

A self-contained desktop colony simulation: shape the terrain, place food, and watch a colony discover, share, and improve routes.

## Download for Windows

[**Download Colony.exe**](https://github.com/SPORstudio-Official/moddelingmym/releases/latest/download/Colony.exe)

This is a portable 64-bit Windows desktop app. Download the single `.exe` and run it—no browser, Python, installation, account, or internet connection is needed. Colony and its simulation assets are bundled inside the executable.

## Field controls

- **Food (1):** left-click to drop a pile. Right-click a pile to remove it. Click nearby again to add more.
- **Obstacle (2):** click to place a rock that workers need to learn around.
- **Eraser (3):** click a food pile or rock to remove it.
- Workers use solid collision hitboxes and steer around one another. If workers collide while either is trying to collect food, it counts as an attack and both workers die; their losses are tracked. Spread food piles apart to lower the risk.
- Pause/resume the simulation and cycle speeds from 1× to 4×.
- **Clear edits** restores the starting field. Relaunching resets the individual ants, but the shared learned brain and exploration map persist on this device.

## A smarter colony

- **Scouts** choose less-visited regions from a shared exploration-memory map; **foragers** exploit remembered sources and follow useful pheromone trails. The queen shifts worker allocation toward gathering when stores run low and toward exploration when food is abundant.
- A shared colony memory lets workers direct one another toward discovered food. Foragers rank sources by distance, remaining yield, learned quality, and crowding, then periodically reconsider assignments. Workers get distinct staging points around each pile, with one reserved picker at a time, to reduce jams.
- Returning workers lay **food pheromone**. Stronger, successful routes persist longer and guide other workers; scent fades over time.
- Workers run weighted **A*** searches over the field, planning waypoints around rock geometry, worker traffic, and useful scent trails; routes re-plan after terrain changes.
- A shared online **Q-learning** policy and each worker's individual action values learn from progress, obstacles, crowding, food collection, and delivery. The compact 64-state policy uses eight compass actions and persists between launches.
- The queen grows the workforce when the nest has enough stored food. Scouts, foragers, food stores, deliveries, and live policy values are visible in the interface.

This is an inspectable simulation of selected planning and memory behaviors—not a literal crow brain or a pretrained neural model. Learned map, source memories, and shared Q-values are stored locally on-device; no data is sent to a server.

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
