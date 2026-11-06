# Colony — Ant Colony Lab

A self-contained desktop colony simulation: shape the terrain, place food, and watch a blind colony explore by touch, memory, and pheromone.

## Download for Windows

[**Download Colony.exe**](https://github.com/SPORstudio-Official/moddelingmym/releases/latest/download/Colony.exe)

This is a portable 64-bit Windows desktop app. Download the single `.exe` and run it—no browser, Python, installation, account, or internet connection is needed. Colony and its simulation assets are bundled inside the executable.

## Field controls

- **Food (1):** left-click to drop a pile. Right-click a pile to remove it. Click nearby again to add more.
- **Obstacle (2):** click to place a rock. Workers cannot see it; they detect it at contact and learn from the bump.
- **Eraser (3):** click a food pile or rock to remove it.
- Workers use solid collision hitboxes and steer around one another. If workers collide while either is trying to collect food, it counts as an attack and both workers die; their losses are tracked. Spread food piles apart to lower the risk.
- Pause/resume the simulation and cycle speeds from 1× to 4×.
- **Clear edits** restores the starting field. Relaunching resets individual ants and pheromone, but shared Q-values and learned food-source memories persist on this device.

## A smarter colony

- The workers have **no visual sensing**. They explore by random walk, each keeping a private visited-area memory; rocks are found by near-body touch probes and collisions, not a global obstacle map.
- **Food is found by body contact** or by detecting a nearby scent trail left by a carrier. Movement decisions never use unseen food coordinates; a worker can recall a source location only after visiting it and detecting its source-coded pheromone again.
- Returning carriers lay directional **food pheromone** pointing back toward the source. Other workers smell the local field, follow the encoded direction, and learn which source produced the trail. Trails fade; successful sources gain quality memory. Their individual reservations and approach slots prevent pileups, while a collision during a contested pickup still kills both workers.
- A worker returning to the nest uses path integration (its internally updated home vector), so it does not need to see the nest. The queen shifts the scout/forager balance toward gathering when stores run low and toward exploration when stores are abundant.
- Shared and individual online **Q-learning** policies learn from progress, scent-following, touch-based obstacle encounters, food collection, and delivery. The compact shared policy has 64 states and eight compass actions and persists between launches.
- The queen grows the workforce when the nest has enough stored food. Scouts, foragers, food stores, deliveries, and live policy values are visible in the interface.

This is an inspectable simulation of selected planning and memory behaviors—not a literal crow brain or a pretrained neural model. Learned source memories and shared Q-values are stored locally on-device; current pheromone trails fade and reset with a fresh simulation. No data is sent to a server.

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
