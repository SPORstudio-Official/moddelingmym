# Colony — Ant Colony Lab

An interactive, browser-native colony simulation. Shape the terrain, add food, and watch workers discover routes and adapt as the colony grows.

## Try it

Open `index.html` in a modern browser, or serve the folder locally:

```bash
python -m http.server 8000
```

Then visit `http://localhost:8000`.

## Controls

- **Food tool (1):** left-click to drop a food pile. Right-click a pile to remove it. Repeated clicks near a pile add more.
- **Obstacle tool (2):** click in the field to place a rock. Workers learn to navigate around it.
- **Eraser (3):** click a food pile or obstacle to remove it.
- **Pause / resume:** stop or restart the simulation. Click the clock-speed control to cycle through 1×, 2×, and 4×.
- **Clear edits:** restore the field to its starting layout.
- **Help (?)** describes the interaction and learning loop.

## The learning system

Each worker uses an online, tabular **Q-learning** policy with 64 compact states (goal bearing, nearby terrain, and whether it is carrying food) and 8 movement actions. Workers receive small rewards for progress, a strong reward for completing a forage or delivery, and a penalty for colliding with an obstacle. Exploration gradually decreases as a worker completes trips. The live policy grid visualizes values being learned during the simulation.

This is a small, interpretable learning system rather than a pretrained neural network. It runs entirely in the browser; there is no server, account, or external model call. The visible colony and its learned policy reset when you refresh the page.
