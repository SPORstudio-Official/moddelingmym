(() => {
  'use strict';

  const W = 1000, H = 620, CELL = 20, GW = Math.ceil(W / CELL), GH = Math.ceil(H / CELL);
  const canvas = document.getElementById('world');
  const ctx = canvas.getContext('2d');
  const chart = document.getElementById('chart');
  const cctx = chart.getContext('2d');
  const $ = (id) => document.getElementById(id);
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const distance = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);
  const fmt = (v) => Math.floor(v).toLocaleString();
  const nest = { x: 292, y: 323, r: 75 };
  const directions = Array.from({ length: 8 }, (_, i) => ({ x: Math.cos(i * Math.PI / 4), y: Math.sin(i * Math.PI / 4) }));
  const foods = [];
  const obstacles = [];
  const workerRadius = 7.5, queenRadius = 12, agentCell = 24;
  let ants = [];
  let agentGrid = new Map();
  let nextAntId = 0;
  let deaths = 0;
  const deathMarks = [];
  let running = true, speed = 1, tool = 'food', frame = 0, simTime = 0;
  let foodStock = 58, delivered = 0, learningUpdates = 0, trips = 0;
  let qTable = Array.from({ length: 64 }, () => Array(8).fill(0));
  let trails = new Float32Array(GW * GH);
  let deliveries = [], chartHistory = [], lastChartTime = 0, lastStatsTime = 0;
  let lastTs = 0, accumulator = 0, animationId = 0, lastToastTimer;
  let pointer = { x: 0, y: 0, inside: false };
  let worldScale = 1, worldOffset = { x: 0, y: 0 };
  let isSmallScreen = matchMedia('(max-width: 560px)').matches;

  const presets = {
    food: [{ x: 808, y: 166, amount: 38, default: true }, { x: 800, y: 492, amount: 30, default: true }],
    rocks: [{ x: 510, y: 235, r: 27, default: true }, { x: 575, y: 279, r: 20, default: true }, { x: 484, y: 408, r: 25, default: true }, { x: 653, y: 366, r: 19, default: true }]
  };

  function resetWorld() {
    foods.splice(0, foods.length, ...presets.food.map(p => ({ ...p })));
    obstacles.splice(0, obstacles.length, ...presets.rocks.map(p => ({ ...p })));
  }

  function spawnAnt(i = ants.length, atNest = true) {
    const angle = Math.random() * Math.PI * 2;
    const x = atNest ? nest.x + Math.cos(angle) * (12 + Math.random() * 27) : nest.x;
    const y = atNest ? nest.y + Math.sin(angle) * (9 + Math.random() * 24) : nest.y;
    const scoutQuota = Math.max(4, Math.ceil((ants.length + 1) * .23));
    const scoutCount = ants.filter(a => a.role === 'scout').length;
    const role = scoutCount < scoutQuota ? 'scout' : 'forager';
    const antId = nextAntId++;
    ants.push({ id: antId, x, y, angle, role, radius: workerRadius, carrying: false, target: null, foodSource: null, roam: null,
      stepTimer: Math.random() * .11, prevState: -1, prevAction: -1, prevDist: 0,
      exploration: role === 'scout' ? .34 + Math.random() * .11 : .10 + Math.random() * .07,
      trips: 0, trailTimer: 0, age: 0 });
  }

  function initialize() {
    resetWorld();
    ants = [];
    nextAntId = 0;
    deaths = 0;
    deathMarks.length = 0;
    for (let i = 0; i < 52; i++) spawnAnt(i);
    drawQGrid();
    resize();
    updateStats(true);
    toast('Colony started · workers are mapping the field');
  }

  function resize() {
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.round(rect.width * dpr);
    canvas.height = Math.round(rect.height * dpr);
    worldScale = Math.min(rect.width / W, rect.height / H);
    worldOffset.x = (rect.width - W * worldScale) / 2;
    worldOffset.y = (rect.height - H * worldScale) / 2;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    draw();
    resizeChart();
  }

  function resizeChart() {
    if (!chart) return;
    const rect = chart.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    chart.width = Math.round(rect.width * dpr);
    chart.height = Math.round(rect.height * dpr);
    cctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    drawChart();
  }

  const observer = new ResizeObserver(resize);
  observer.observe(canvas.parentElement);
  window.addEventListener('resize', resizeChart);

  function worldPoint(event) {
    const rect = canvas.getBoundingClientRect();
    const x = (event.clientX - rect.left - worldOffset.x) / worldScale;
    const y = (event.clientY - rect.top - worldOffset.y) / worldScale;
    return { x: clamp(x, 0, W), y: clamp(y, 0, H) };
  }

  function nearest(list, p, maxDist = Infinity) {
    let result = null, best = maxDist;
    for (const item of list) {
      const d = distance(item, p);
      if (d < best) { best = d; result = item; }
    }
    return result;
  }

  function releaseTarget(ant) {
    if (ant.target) {
      ant.target.claimedBy = Math.max(0, (ant.target.claimedBy || 1) - 1);
      if (ant.target.reservedBy === ant) ant.target.reservedBy = null;
      if (ant.approachSlot != null && ant.target.slotAssignments) ant.target.slotAssignments.delete(ant.approachSlot);
    }
    ant.target = null;
    ant.approach = null;
    ant.approachSlot = null;
  }

  function gridKey(x, y) {
    return `${Math.floor(x / agentCell)},${Math.floor(y / agentCell)}`;
  }

  function rebuildAgentGrid() {
    const grid = new Map();
    for (const ant of ants) {
      const key = gridKey(ant.x, ant.y);
      if (!grid.has(key)) grid.set(key, []);
      grid.get(key).push(ant);
    }
    return grid;
  }

  function agentsNear(x, y, exclude = null) {
    const found = [];
    const gx = Math.floor(x / agentCell), gy = Math.floor(y / agentCell);
    for (let cy = gy - 1; cy <= gy + 1; cy++) for (let cx = gx - 1; cx <= gx + 1; cx++) {
      const group = agentGrid.get(`${cx},${cy}`);
      if (group) for (const ant of group) if (ant !== exclude) found.push(ant);
    }
    return found;
  }

  function workerOccupies(x, y, ant) {
    if (Math.hypot(x - (nest.x - 3), y - (nest.y + 8)) < (ant?.radius || workerRadius) + queenRadius) return true;
    return agentsNear(x, y, ant).some(other => Math.hypot(x - other.x, y - other.y) < (ant?.radius || workerRadius) + other.radius + 1.5);
  }

  function crowdPenaltyAt(x, y, ant) {
    let penalty = 0;
    const safe = (ant?.radius || workerRadius) * 2 + 10;
    const queenDistance = Math.hypot(x - (nest.x - 3), y - (nest.y + 8));
    penalty = Math.max(penalty, clamp((safe - queenDistance) / safe, 0, 1));
    for (const other of agentsNear(x, y, ant)) {
      const d = Math.hypot(x - other.x, y - other.y);
      penalty = Math.max(penalty, clamp((safe - d) / safe, 0, 1));
    }
    return penalty;
  }

  function addFood(p) {
    const existing = nearest(foods, p, 34);
    if (existing) {
      existing.amount = Math.min(120, existing.amount + 20);
      existing.default = false;
    } else foods.push({ x: p.x, y: p.y, amount: 24, default: false });
    toast('Food dropped · the colony can smell it');
    hintForTool();
  }

  function removeFood(p) {
    const food = nearest(foods, p, 48);
    if (!food) { toast('No food pile close enough to remove'); return; }
    foods.splice(foods.indexOf(food), 1);
    for (const ant of ants) if (ant.target === food) releaseTarget(ant);
    toast('Food pile removed');
  }

  function addObstacle(p) {
    if (distance(p, nest) < nest.r + 19) { toast('Keep rocks clear of the colony entrance'); return; }
    if (nearest(obstacles, p, 47)) { toast('There is already a rock nearby'); return; }
    obstacles.push({ x: p.x, y: p.y, r: 21 + Math.random() * 12, default: false, seed: Math.random() * 100 });
    toast('Rock placed · workers will learn to route around it');
    hintForTool();
  }

  function eraseAt(p) {
    const f = nearest(foods, p, 46), o = nearest(obstacles, p, 47);
    if (f && (!o || distance(f, p) < distance(o, p))) removeFood(p);
    else if (o) { obstacles.splice(obstacles.indexOf(o), 1); toast('Obstacle erased'); }
    else toast('Nothing under the eraser');
  }

  canvas.addEventListener('pointermove', (e) => {
    pointer = { ...worldPoint(e), inside: true };
  });
  canvas.addEventListener('pointerleave', () => { pointer.inside = false; });
  canvas.addEventListener('pointerdown', (e) => {
    if (e.button === 2) { e.preventDefault(); removeFood(worldPoint(e)); return; }
    if (e.button !== 0) return;
    const p = worldPoint(e);
    if (tool === 'food') addFood(p);
    else if (tool === 'obstacle') addObstacle(p);
    else if (tool === 'eraser') eraseAt(p);
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());

  function setTool(name) {
    tool = name;
    document.querySelectorAll('.tool-button').forEach(b => b.classList.toggle('active', b.dataset.tool === name));
    const messages = { food: 'Click anywhere to drop food', obstacle: 'Click to place a rock', eraser: 'Click a food pile or rock to erase' };
    $('canvasHint').innerHTML = `<span class="hint-mouse">↖</span> ${messages[name]}`;
    $('canvasHint').classList.remove('hidden');
  }

  function hintForTool() { window.setTimeout(() => $('canvasHint').classList.add('hidden'), 1800); }
  document.querySelectorAll('.tool-button').forEach(b => b.addEventListener('click', () => setTool(b.dataset.tool)));
  document.addEventListener('keydown', (e) => {
    if (e.target.matches('input,textarea,select') || $('helpDialog').open) return;
    if (e.key === '1') setTool('food');
    if (e.key === '2') setTool('obstacle');
    if (e.key === '3') setTool('eraser');
    if (e.code === 'Space') { e.preventDefault(); toggleRun(); }
  });

  function toggleRun() {
    running = !running;
    $('playButton').innerHTML = `<span class="play-symbol">${running ? 'Ⅱ' : '▶'}</span><span>${running ? 'Pause' : 'Resume'}</span>`;
    $('runLabel').textContent = running ? 'SIMULATION RUNNING' : 'SIMULATION PAUSED';
    $('liveDot').classList.toggle('paused', !running);
    if (!running) $('canvasHint').classList.add('hidden');
    toast(running ? 'Colony is moving again' : 'Simulation paused');
  }
  $('playButton').addEventListener('click', toggleRun);
  $('clearButton').addEventListener('click', () => {
    resetWorld();
    for (const ant of ants) { ant.target = null; ant.approach = null; ant.roam = null; }
    toast('Field reset to its starting layout');
  });
  $('helpButton').addEventListener('click', () => $('helpDialog').showModal());
  $('helpDialog').addEventListener('click', (e) => { if (e.target === $('helpDialog')) $('helpDialog').close(); });

  function obstacleBlocked(x, y, radius = 6) {
    if (x < 16 || y < 16 || x > W - 16 || y > H - 16) return true;
    for (const o of obstacles) if (Math.hypot(o.x - x, o.y - y) < o.r + radius) return true;
    return false;
  }

  function foodPickupOccupied(food, exclude) {
    const owner = food.reservedBy;
    if (owner && owner !== exclude) {
      const stillValid = ants.includes(owner) && (owner.target === food || (owner.carrying && owner.foodSource === food));
      if (stillValid) return true;
      food.reservedBy = null;
    }
    return ants.some(other => other !== exclude && other.carrying && other.foodSource === food && distance(other, food) < 18);
  }

  function foodApproach(ant) {
    if (!ant.target || !ant.approach) return ant.target;
    if (distance(ant, ant.target) < 27 && !foodPickupOccupied(ant.target, ant)) ant.target.reservedBy = ant;
    if (foodPickupOccupied(ant.target, ant)) return ant.approach;
    if (distance(ant, ant.approach) > 12) return ant.approach;
    return ant.target;
  }

  function targetFor(ant) {
    if (ant.carrying) return nest;
    const validTarget = ant.target && foods.includes(ant.target) && ant.target.amount > 0;
    if (validTarget && (ant.role === 'scout' || simTime - (ant.targetSince || 0) < 15)) return foodApproach(ant);
    const previousTarget = ant.target, previousApproach = ant.approach, previousSlot = ant.approachSlot;
    releaseTarget(ant);
    // Scouts explore beyond colony memory; foragers rank discovered sources by yield, quality, and congestion.
    let best = null, bestScore = Infinity;
    for (const f of foods) {
      if (f.amount <= 0) continue;
      const d = distance(ant, f);
      if (f.discovered || d < 76) {
        f.discovered = true;
        const quality = f.quality ?? .45;
        const scoutDetour = ant.role === 'scout' && d > 125 ? 210 : 0;
        const crowding = Math.max(0, (f.claimedBy || 0) - 2) * 55;
        const score = d * (1.14 - quality * .34) - Math.min(72, f.amount) * .42 + crowding + scoutDetour;
        if (score < bestScore) { best = f; bestScore = score; }
      }
    }
    if (best) {
      ant.target = best; ant.targetSince = simTime; best.claimedBy = (best.claimedBy || 0) + 1;
      if (best === previousTarget && previousApproach && previousSlot != null) {
        ant.approach = previousApproach; ant.approachSlot = previousSlot;
        if (!best.slotAssignments) best.slotAssignments = new Set();
        best.slotAssignments.add(previousSlot);
      } else {
        if (!best.slotAssignments) best.slotAssignments = new Set();
        let slot = 0; while (best.slotAssignments.has(slot)) slot++;
        best.slotAssignments.add(slot); ant.approachSlot = slot;
        const angle = (slot % 8) * Math.PI / 4 + (best.seed || 0);
        const ring = 29 + Math.floor(slot / 8) * 14;
        ant.approach = { x: clamp(best.x + Math.cos(angle) * ring, 24, W - 24), y: clamp(best.y + Math.sin(angle) * ring, 24, H - 24) };
      }
      return foodApproach(ant);
    }
    if (!ant.roam || distance(ant, ant.roam) < 35 || obstacleBlocked(ant.roam.x, ant.roam.y, 10)) {
      const a = ant.angle + (Math.random() - .5) * 2.8;
      const range = 110 + Math.random() * 170;
      ant.roam = { x: clamp(ant.x + Math.cos(a) * range, 32, W - 32), y: clamp(ant.y + Math.sin(a) * range, 32, H - 32) };
    }
    return ant.roam;
  }

  function stateFor(ant, target) {
    const angle = Math.atan2(target.y - ant.y, target.x - ant.x);
    const bearing = ((Math.round(angle / (Math.PI / 4)) % 8) + 8) % 8;
    const ux = Math.cos(angle), uy = Math.sin(angle);
    const ahead = obstacles.some(o => Math.hypot(o.x - (ant.x + ux * 34), o.y - (ant.y + uy * 34)) < o.r + 16) ||
      workerOccupies(ant.x + ux * 30, ant.y + uy * 30, ant);
    const left = obstacles.some(o => Math.hypot(o.x - (ant.x - uy * 29), o.y - (ant.y + ux * 29)) < o.r + 12);
    const right = obstacles.some(o => Math.hypot(o.x - (ant.x + uy * 29), o.y - (ant.y - ux * 29)) < o.r + 12);
    let terrain = ahead ? 1 : 0;
    if (!ahead && left !== right) terrain = left ? 2 : 3;
    return (ant.carrying ? 32 : 0) + bearing * 4 + terrain;
  }

  function trailAt(x, y) {
    const gx = clamp(Math.round(x / CELL), 0, GW - 1), gy = clamp(Math.round(y / CELL), 0, GH - 1);
    return trails[gy * GW + gx];
  }

  function chooseAction(ant, target, state) {
    const vx = target.x - ant.x, vy = target.y - ant.y, d = Math.max(1, Math.hypot(vx, vy));
    let bestAction = 0, bestScore = -Infinity;
    const explore = Math.max(.045, ant.exploration * (1 - Math.min(.7, simTime / 900)));
    const randomExplore = Math.random() < explore;
    const order = directions.map((_, i) => i).sort(() => Math.random() - .5);
    for (const i of order) {
      const dir = directions[i];
      const nx = ant.x + dir.x * 10, ny = ant.y + dir.y * 10;
      const blocked = obstacleBlocked(nx, ny, 7);
      let score = (dir.x * vx + dir.y * vy) / d * 2.1;
      score += clamp(qTable[state][i], -1, 1) * .18;
      if (!ant.carrying) score += (trailAt(nx, ny) - trailAt(ant.x - dir.x * 10, ant.y - dir.y * 10)) * .62;
      if (blocked) score -= 2.4;
      score -= crowdPenaltyAt(nx, ny, ant) * 2.6;
      score += (Math.random() - .5) * (randomExplore ? 1.35 : .13);
      if (score > bestScore) { bestScore = score; bestAction = i; }
    }
    return bestAction;
  }

  function updateQ(ant, state, action, reward, nextState, terminal = false) {
    if (ant.prevState >= 0 && ant.prevAction >= 0) {
      const row = qTable[ant.prevState];
      const bestFuture = terminal ? 0 : Math.max(...qTable[nextState]);
      const old = row[ant.prevAction];
      row[ant.prevAction] = old + .12 * (reward + .84 * bestFuture - old);
      learningUpdates++;
    }
    ant.prevState = state;
    ant.prevAction = action;
  }

  function moveAnt(ant, dt) {
    ant.age += dt;
    ant.stepTimer -= dt;
    const target = targetFor(ant);
    const distBefore = distance(ant, target);
    const state = stateFor(ant, target);
    if (ant.stepTimer <= 0) {
      const action = chooseAction(ant, target, state);
      ant.angle = Math.atan2(directions[action].y, directions[action].x);
      ant.action = action;
      ant.policyState = state;
      ant.stepTimer = .12 + Math.random() * .11;
    }
    const dir = directions[ant.action ?? Math.round(ant.angle / (Math.PI / 4) + 8) % 8];
    const pace = ant.carrying ? 37 : 43;
    const nx = ant.x + dir.x * pace * dt;
    const ny = ant.y + dir.y * pace * dt;
    let reward = 0;
    const rockBlocked = obstacleBlocked(nx, ny, 6);
    if (rockBlocked) {
      reward -= .9;
      ant.angle += (Math.random() < .5 ? -1 : 1) * .6;
      ant.stepTimer = 0;
    } else {
      // Workers may press into a crowd; the collision solver separates bodies immediately after movement.
      ant.x = nx; ant.y = ny;
      const distAfter = distance(ant, target);
      reward += clamp((distBefore - distAfter) / 8, -.14, .14);
    }
    ant.x = clamp(ant.x, 17, W - 17); ant.y = clamp(ant.y, 17, H - 17);
    const nextState = stateFor(ant, target);
    updateQ(ant, ant.policyState ?? state, ant.action ?? 0, reward, nextState);

    ant.trailTimer -= dt;
    if (ant.carrying && ant.trailTimer <= 0) {
      const gx = clamp(Math.floor(ant.x / CELL), 0, GW - 1), gy = clamp(Math.floor(ant.y / CELL), 0, GH - 1);
      const sourceQuality = ant.foodSource?.quality ?? .45;
      trails[gy * GW + gx] = Math.min(1, trails[gy * GW + gx] + .20 + sourceQuality * .20);
      ant.trailTimer = .08;
    }

    if (!ant.carrying && ant.target && foods.includes(ant.target) && distance(ant, ant.target) < 17 && ant.target.amount > 0 && !foodPickupOccupied(ant.target, ant)) {
      const source = ant.target;
      source.amount--;
      source.discovered = true;
      source.visits = (source.visits || 0) + 1;
      ant.foodSource = source;
      ant.pickupSource = source;
      ant.pickupAt = simTime;
      ant.tripStartedAt = simTime;
      ant.carrying = true;
      ant.roam = null;
      updateQ(ant, nextState, ant.action ?? 0, 4, nextState, true);
      releaseTarget(ant);
      if (source.amount <= 0) {
        foods.splice(foods.indexOf(source), 1);
        for (const worker of ants) if (worker.target === source) releaseTarget(worker);
      }
      ant.stepTimer = 0;
    } else if (ant.carrying && distance(ant, nest) < nest.r * .68) {
      ant.carrying = false;
      ant.roam = null;
      if (ant.foodSource) {
        const source = ant.foodSource;
        const routeEfficiency = clamp(1 - (simTime - (ant.tripStartedAt || simTime)) / 105, .04, 1);
        source.quality = (source.quality ?? .45) * .72 + routeEfficiency * .28;
        source.returns = (source.returns || 0) + 1;
        ant.foodSource = null;
      }
      foodStock++;
      delivered++;
      trips++;
      deliveries.push(simTime);
      updateQ(ant, nextState, ant.action ?? 0, 6, nextState, true);
      ant.stepTimer = 0;
      ant.exploration = ant.role === 'scout' ? Math.max(.2, ant.exploration * .998) : Math.max(.035, ant.exploration * .993);
    }
  }

  function diffuseTrails(dt) {
    const fade = Math.pow(.992, dt * 60);
    for (let i = 0; i < trails.length; i++) trails[i] *= fade;
  }

  function isPickupContestant(ant) {
    const reachingFood = !ant.carrying && ant.target && foods.includes(ant.target) && distance(ant, ant.target) < 18;
    const justPicked = ant.pickupSource && simTime - (ant.pickupAt ?? -Infinity) < .001 && distance(ant, ant.pickupSource) < 18;
    return !!(reachingFood || justPicked);
  }

  function resolveWorkerCollisions() {
    const dead = new Set();
    for (let pass = 0; pass < 2; pass++) {
      agentGrid = rebuildAgentGrid();
      for (const ant of ants) {
        if (dead.has(ant)) continue;
        for (const other of agentsNear(ant.x, ant.y, ant)) {
          if (other.id <= ant.id || dead.has(other)) continue;
          let dx = other.x - ant.x, dy = other.y - ant.y;
          let d = Math.hypot(dx, dy);
          const minDistance = ant.radius + other.radius;
          const foodContact = d < minDistance && (isPickupContestant(ant) || isPickupContestant(other));
          if (foodContact) {
            dead.add(ant); dead.add(other);
            continue;
          }
          if (d >= minDistance) continue;
          updateQ(ant, ant.policyState ?? 0, ant.action ?? 0, -.55, ant.policyState ?? 0);
          updateQ(other, other.policyState ?? 0, other.action ?? 0, -.55, other.policyState ?? 0);
          if (d < .001) { const a = ((ant.id * 37 + other.id * 13) % 360) * Math.PI / 180; dx = Math.cos(a); dy = Math.sin(a); d = 1; }
          const separation = (minDistance - d + .08) * .51;
          const ux = dx / d, uy = dy / d;
          ant.x -= ux * separation; ant.y -= uy * separation;
          other.x += ux * separation; other.y += uy * separation;
        }
      }
    }
    // The queen is a fixed, non-combatant body; workers yield her space.
    const queen = { x: nest.x - 3, y: nest.y + 8 };
    for (const ant of ants) {
      let dx = ant.x - queen.x, dy = ant.y - queen.y;
      let d = Math.hypot(dx, dy), minDistance = ant.radius + queenRadius;
      if (d < minDistance) {
        if (d < .001) { dx = Math.cos(ant.id); dy = Math.sin(ant.id); d = 1; }
        const shift = minDistance - d + .1;
        ant.x += dx / d * shift; ant.y += dy / d * shift;
      }
    }
    if (dead.size) {
      for (const ant of dead) {
        updateQ(ant, ant.policyState ?? 0, ant.action ?? 0, -4.5, ant.policyState ?? 0, true);
        releaseTarget(ant);
        if (ant.carrying) foods.push({ x: ant.x, y: ant.y, amount: 1, quality: ant.foodSource?.quality ?? .45, discovered: true, default: false });
        deathMarks.push({ x: ant.x, y: ant.y, at: simTime });
      }
      ants = ants.filter(ant => !dead.has(ant));
      deaths += dead.size;
      toast(`${dead.size} workers lost in a food-pile fight · spread out food to prevent clashes`);
    }
    agentGrid = rebuildAgentGrid();
  }

  function update(dt) {
    simTime += dt;
    frame++;
    diffuseTrails(dt);
    agentGrid = rebuildAgentGrid();
    for (const ant of ants) moveAnt(ant, dt);
    resolveWorkerCollisions();
    for (let i = deathMarks.length - 1; i >= 0; i--) if (simTime - deathMarks[i].at > 1.6) deathMarks.splice(i, 1);
    // A well-provisioned nest can support another worker; the queen lays slowly.
    if (simTime % 34 < dt && ants.length < 96 && foodStock >= 10) {
      foodStock -= 10;
      spawnAnt(ants.length);
      toast('The queen has raised a new worker');
    }
    if (simTime - lastChartTime >= 2) {
      chartHistory.push({ time: simTime, delivered });
      if (chartHistory.length > 35) chartHistory.shift();
      lastChartTime = simTime;
    }
    if (simTime - lastStatsTime >= .35) {
      updateStats(); lastStatsTime = simTime;
    }
    if (frame % 28 === 0) drawQGrid();
  }

  function updateStats(force = false) {
    $('populationStat').textContent = fmt(ants.length + 1);
    const scouts = ants.filter(a => a.role === 'scout').length;
    $('populationFoot').textContent = `${scouts} SCOUTS · ${ants.length - scouts} FORAGERS`;
    $('foodStat').textContent = fmt(foodStock);
    $('deliveredStat').textContent = fmt(delivered);
    $('learningStat').textContent = fmt(learningUpdates);
    $('lossCount').textContent = `${fmt(deaths)} LOST`;
    $('lossCount').classList.toggle('has-losses', deaths > 0);
    const mm = Math.floor(simTime / 60), ss = Math.floor(simTime % 60);
    $('timeLabel').textContent = `DAY ${String(Math.floor(mm / 12) + 1).padStart(2, '0')} · ${String(mm % 12).padStart(2, '0')}:${String(ss).padStart(2, '0')}`;
    const windowStart = simTime - 60;
    const recent = deliveries.filter(t => t >= windowStart).length;
    $('rateLabel').textContent = `${Math.round(recent)} / MIN`;
    $('chartEmpty').style.display = delivered > 0 ? 'none' : 'grid';
    if (force) drawChart();
  }

  function drawQGrid() {
    const grid = $('qGrid');
    if (!grid) return;
    if (grid.children.length === 0) {
      for (let i = 0; i < 64; i++) { const cell = document.createElement('i'); cell.className = 'q-cell'; grid.appendChild(cell); }
    }
    const cells = grid.children;
    for (let s = 0; s < 64; s++) {
      const vals = qTable[s];
      const max = Math.max(...vals), min = Math.min(...vals);
      const cell = cells[s];
      cell.classList.toggle('strong', max > .28);
      cell.classList.toggle('learned', max > .035 && max <= .28);
      cell.classList.toggle('negative', min < -.16);
      cell.title = `State ${s + 1}: best action value ${max.toFixed(2)}`;
    }
  }

  function drawBackground() {
    ctx.fillStyle = '#f0f2e9'; ctx.fillRect(0, 0, W, H);
    ctx.save();
    ctx.strokeStyle = '#e3e8dd'; ctx.lineWidth = 1;
    for (let x = 40; x < W; x += 40) for (let y = 40; y < H; y += 40) {
      ctx.beginPath(); ctx.arc(x, y, .75, 0, Math.PI * 2); ctx.fillStyle = '#dfe5d9'; ctx.fill();
    }
    // Soft meadow clearings
    const glow = ctx.createRadialGradient(600, 310, 40, 600, 310, 450);
    glow.addColorStop(0, '#f7f7ef55'); glow.addColorStop(1, '#edf0e800');
    ctx.fillStyle = glow; ctx.fillRect(0, 0, W, H);
    // Low, quiet terrain contour lines
    ctx.strokeStyle = '#dfe5d959'; ctx.lineWidth = 1;
    [[150,150,72],[805,315,95],[610,84,68],[910,555,66],[105,490,57]].forEach(([x,y,r])=>{
      ctx.beginPath();ctx.ellipse(x,y,r*1.4,r*.66,-.35,0,Math.PI*2);ctx.stroke();
      ctx.beginPath();ctx.ellipse(x,y,r*1.8,r*.85,-.35,0,Math.PI*2);ctx.stroke();
    });
    ctx.restore();
  }

  function drawTrails() {
    for (let gy = 0; gy < GH; gy++) for (let gx = 0; gx < GW; gx++) {
      const v = trails[gy * GW + gx];
      if (v < .07) continue;
      ctx.fillStyle = `rgba(42,115,80,${Math.min(.18, v * .16)})`;
      ctx.beginPath(); ctx.arc(gx * CELL + CELL / 2, gy * CELL + CELL / 2, 3 + v * 2, 0, Math.PI * 2); ctx.fill();
    }
  }

  function drawNest() {
    // Underground mound and entrance
    ctx.save();
    ctx.shadowColor = '#69785b23';ctx.shadowBlur = 20;ctx.shadowOffsetY = 8;
    ctx.fillStyle = '#d7dbc7';ctx.beginPath();ctx.ellipse(nest.x,nest.y+2,82,71,-.14,0,Math.PI*2);ctx.fill();
    ctx.restore();
    ctx.fillStyle = '#c4c9b4';ctx.beginPath();ctx.ellipse(nest.x,nest.y+8,69,56,-.14,0,Math.PI*2);ctx.fill();
    ctx.fillStyle = '#394b3e';ctx.beginPath();ctx.ellipse(nest.x,nest.y+9,45,36,-.12,0,Math.PI*2);ctx.fill();
    ctx.fillStyle = '#23382d';ctx.beginPath();ctx.ellipse(nest.x,nest.y+9,37,29,-.12,0,Math.PI*2);ctx.fill();
    // Nest chambers
    ctx.strokeStyle='#6c816b66';ctx.lineWidth=1.2;
    [[nest.x-16,nest.y-4,10,7],[nest.x+15,nest.y+14,13,8],[nest.x-3,nest.y+27,8,5]].forEach(([x,y,rx,ry])=>{ctx.beginPath();ctx.ellipse(x,y,rx,ry,0,0,Math.PI*2);ctx.stroke()});
    // Earth flecks around the rim
    for(let i=0;i<14;i++){const a=i*Math.PI*2/14;ctx.fillStyle=i%2?'#9caa89':'#b7bf9f';ctx.beginPath();ctx.ellipse(nest.x+Math.cos(a)*73,nest.y+Math.sin(a)*59,5,3,a,0,Math.PI*2);ctx.fill()}
    // Entrance tunnel
    ctx.strokeStyle='#f0f2e9';ctx.lineWidth=11;ctx.lineCap='round';ctx.beginPath();ctx.moveTo(nest.x+31,nest.y+1);ctx.quadraticCurveTo(nest.x+55,nest.y-3,nest.x+68,nest.y-2);ctx.stroke();
    ctx.strokeStyle='#374c3d';ctx.lineWidth=5;ctx.beginPath();ctx.moveTo(nest.x+31,nest.y+1);ctx.quadraticCurveTo(nest.x+55,nest.y-3,nest.x+68,nest.y-2);ctx.stroke();
    // Queen in chamber
    drawAnt({x:nest.x-3,y:nest.y+8,angle:-.18,queen:true,carrying:false});
    // Label
    ctx.fillStyle='#6e7c70';ctx.font='9px "Segoe UI", monospace';ctx.textAlign='center';ctx.fillText('NEST · QUEEN',nest.x,nest.y+76);
  }

  function drawAnt(ant) {
    ctx.save();ctx.translate(ant.x,ant.y);ctx.rotate(ant.angle||0);
    const s=ant.queen?1.7:1;
    ctx.lineCap='round';ctx.strokeStyle=ant.queen?'#b9792d':'#304d3c';ctx.lineWidth=1.1*s;
    // legs animate lightly
    const swing=Math.sin(simTime*15+ant.id)*2.1*s;
    for(const side of [-1,1]) for(const off of [-3,1,5]){
      ctx.beginPath();ctx.moveTo(off*s,side*1.5*s);ctx.lineTo((off-1)*s,side*(4+swing*.35)*s);ctx.lineTo((off-3)*s,side*(5+swing)*s);ctx.stroke();
    }
    ctx.fillStyle=ant.queen?'#e1a44e':ant.role==='scout'?'#78945d':'#355a43';
    ctx.beginPath();ctx.ellipse(-4.5*s,0,3.8*s,3*s,0,0,Math.PI*2);ctx.fill();
    ctx.beginPath();ctx.ellipse(0,0,2.1*s,1.9*s,0,0,Math.PI*2);ctx.fill();
    ctx.beginPath();ctx.arc(4*s,0,2.1*s,0,Math.PI*2);ctx.fill();
    if(ant.queen){ctx.fillStyle='#f5d884';ctx.beginPath();ctx.moveTo(2*s,-4*s);ctx.lineTo(2*s,-8*s);ctx.lineTo(4*s,-6*s);ctx.lineTo(6*s,-8*s);ctx.lineTo(6*s,-4*s);ctx.closePath();ctx.fill()}
    if(ant.carrying){ctx.fillStyle='#d5a24c';ctx.beginPath();ctx.arc(-8*s,-3.6*s,2.2*s,0,Math.PI*2);ctx.fill()}
    ctx.restore();
  }

  function drawFood(food) {
    const pileSize=4+Math.min(5,Math.sqrt(food.amount)*.72);
    ctx.save();ctx.translate(food.x,food.y);
    ctx.fillStyle='#d9d6be';ctx.beginPath();ctx.ellipse(0,7,pileSize*1.55,pileSize*.73,0,0,Math.PI*2);ctx.fill();
    const seeds=Math.min(8,Math.max(3,Math.ceil(food.amount/7)));
    for(let i=0;i<seeds;i++){
      const a=i*2.399, r=Math.sqrt(i)*pileSize*.37;
      ctx.fillStyle=i%3===0?'#f1cc71':i%3===1?'#c78e3c':'#e6b354';
      ctx.beginPath();ctx.ellipse(Math.cos(a)*r,Math.sin(a)*r,2.8,2, a,0,Math.PI*2);ctx.fill();
      ctx.fillStyle='#fff5cf88';ctx.beginPath();ctx.arc(Math.cos(a)*r-.7,Math.sin(a)*r-.5,.65,0,Math.PI*2);ctx.fill();
    }
    ctx.restore();
  }

  function drawObstacle(o) {
    ctx.save();ctx.translate(o.x,o.y);
    ctx.fillStyle='#55625720';ctx.beginPath();ctx.ellipse(2,o.r*.72,o.r*1.02,o.r*.48,-.15,0,Math.PI*2);ctx.fill();
    ctx.fillStyle='#8b9484';ctx.beginPath();ctx.moveTo(-o.r*.9,o.r*.27);ctx.lineTo(-o.r*.72,-o.r*.36);ctx.lineTo(-o.r*.18,-o.r*.91);ctx.lineTo(o.r*.52,-o.r*.72);ctx.lineTo(o.r*.96,-o.r*.17);ctx.lineTo(o.r*.76,o.r*.36);ctx.lineTo(o.r*.13,o.r*.61);ctx.closePath();ctx.fill();
    ctx.fillStyle='#abb29f';ctx.beginPath();ctx.moveTo(-o.r*.72,-o.r*.36);ctx.lineTo(-o.r*.18,-o.r*.91);ctx.lineTo(o.r*.52,-o.r*.72);ctx.lineTo(o.r*.16,-o.r*.32);ctx.lineTo(-o.r*.26,-o.r*.2);ctx.closePath();ctx.fill();
    ctx.fillStyle='#ced1bc';ctx.beginPath();ctx.moveTo(-o.r*.18,-o.r*.91);ctx.lineTo(o.r*.52,-o.r*.72);ctx.lineTo(o.r*.16,-o.r*.32);ctx.lineTo(-o.r*.1,-o.r*.38);ctx.closePath();ctx.fill();
    ctx.restore();
  }

  function drawDeathMarks() {
    for (const mark of deathMarks) {
      const fade = clamp(1 - (simTime - mark.at) / 1.6, 0, 1);
      ctx.save(); ctx.globalAlpha = fade * .62; ctx.translate(mark.x, mark.y);
      ctx.strokeStyle = '#b45c4d'; ctx.lineWidth = 2.2; ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(-4,-4); ctx.lineTo(4,4); ctx.moveTo(4,-4); ctx.lineTo(-4,4); ctx.stroke();
      ctx.restore();
    }
  }

  function draw() {
    if(!canvas.width||!canvas.height)return;
    const rect=canvas.getBoundingClientRect();
    ctx.setTransform(window.devicePixelRatio||1,0,0,window.devicePixelRatio||1,0,0);
    ctx.clearRect(0,0,rect.width,rect.height);
    ctx.translate(worldOffset.x,worldOffset.y);ctx.scale(worldScale,worldScale);
    drawBackground();drawTrails();
    // Draw faint workers' explored-area flecks
    drawNest();
    for(const o of obstacles)drawObstacle(o);
    for(const f of foods)drawFood(f);
    drawDeathMarks();
    for(const ant of ants)drawAnt(ant);
    if(pointer.inside){
      ctx.save();ctx.globalAlpha=.65;
      if(tool==='food'){
        ctx.strokeStyle='#b88b45';ctx.setLineDash([4,4]);ctx.beginPath();ctx.arc(pointer.x,pointer.y,19,0,Math.PI*2);ctx.stroke();
        ctx.fillStyle='#d7a950';ctx.font='17px sans-serif';ctx.textAlign='center';ctx.fillText('+',pointer.x,pointer.y+5);
      }else if(tool==='obstacle'){
        ctx.fillStyle='#68766b38';ctx.strokeStyle='#68766b';ctx.setLineDash([4,3]);ctx.beginPath();ctx.arc(pointer.x,pointer.y,27,0,Math.PI*2);ctx.fill();ctx.stroke();
      }else{
        ctx.strokeStyle='#a27fb2';ctx.setLineDash([3,3]);ctx.beginPath();ctx.arc(pointer.x,pointer.y,23,0,Math.PI*2);ctx.stroke();
      }
      ctx.restore();
    }
    ctx.setTransform(window.devicePixelRatio||1,0,0,window.devicePixelRatio||1,0,0);
    drawChart();
  }

  function drawChart() {
    if(!chart.width||!chart.height)return;
    const rect=chart.getBoundingClientRect(), dpr=Math.min(window.devicePixelRatio||1,2);
    cctx.setTransform(dpr,0,0,dpr,0,0);cctx.clearRect(0,0,rect.width,rect.height);
    const pad={l:4,r:4,t:7,b:8},w=rect.width-pad.l-pad.r,h=rect.height-pad.t-pad.b;
    cctx.strokeStyle='#edf0eb';cctx.lineWidth=1;
    for(let i=0;i<3;i++){const y=pad.t+h*i/2;cctx.beginPath();cctx.moveTo(pad.l,y);cctx.lineTo(pad.l+w,y);cctx.stroke()}
    if(chartHistory.length<2)return;
    const vals=chartHistory.map(x=>x.delivered), min=Math.min(...vals), max=Math.max(...vals), spread=Math.max(4,max-min);
    const points=chartHistory.map((p,i)=>({x:pad.l+w*i/(chartHistory.length-1),y:pad.t+h-(p.delivered-min+1)/ (spread+1)*h}));
    cctx.beginPath();cctx.moveTo(points[0].x,pad.t+h);for(const p of points)cctx.lineTo(p.x,p.y);cctx.lineTo(points.at(-1).x,pad.t+h);cctx.closePath();
    const g=cctx.createLinearGradient(0,pad.t,0,pad.t+h);g.addColorStop(0,'#72b8894d');g.addColorStop(1,'#72b88900');cctx.fillStyle=g;cctx.fill();
    cctx.beginPath();points.forEach((p,i)=>i?cctx.lineTo(p.x,p.y):cctx.moveTo(p.x,p.y));cctx.strokeStyle='#4c9b72';cctx.lineWidth=1.8;cctx.lineJoin='round';cctx.lineCap='round';cctx.stroke();
    const last=points.at(-1);cctx.fillStyle='#4c9b72';cctx.beginPath();cctx.arc(last.x,last.y,2.7,0,Math.PI*2);cctx.fill();
  }

  function toast(text) {
    const el=$('toast');el.textContent=text;el.classList.add('show');
    clearTimeout(lastToastTimer);lastToastTimer=setTimeout(()=>el.classList.remove('show'),2100);
  }

  function loop(ts) {
    if(!lastTs)lastTs=ts;
    const elapsed=Math.min(.05,(ts-lastTs)/1000);lastTs=ts;
    if(running){accumulator+=elapsed*speed;let steps=0;while(accumulator>=1/60&&steps<6){update(1/60);accumulator-=1/60;steps++}}
    draw();
    animationId=requestAnimationFrame(loop);
  }

  // Convenience speed cycling from the field label.
  const speedControl=document.createElement('button');
  speedControl.type='button';speedControl.className='speed-control';speedControl.title='Change simulation speed';speedControl.innerHTML='<span class="speed-glyph">◷</span> <span id="speedText">1×</span>';
  speedControl.addEventListener('click',()=>{const values=[1,2,4];speed=values[(values.indexOf(speed)+1)%values.length];$('speedText').textContent=`${speed}×`;toast(`Simulation speed · ${speed}×`)});
  const worldActions=document.querySelector('.world-actions');
  const divider=document.createElement('span');divider.className='action-divider';worldActions.insertBefore(speedControl,worldActions.querySelector('#playButton'));worldActions.insertBefore(divider,worldActions.querySelector('#playButton'));

  initialize();
  animationId=requestAnimationFrame(loop);
})();
