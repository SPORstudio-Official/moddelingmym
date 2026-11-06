(() => {
  'use strict';

  const SAVE_KEY = 'wonderworks.arcade.save.v1';
  const $ = (id) => document.getElementById(id);
  const pick = (items) => items[Math.floor(Math.random() * items.length)];
  const shuffle = (items) => [...items].sort(() => Math.random() - .5);
  const escapeHTML = (value) => String(value).replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[char]));
  const promptDeck = [
    { title: 'Make an ordinary wait feel magical.', copy: 'A small moment can become a whole new world. Find an unexpected way in.', mood: 'PLAYFUL THINKING' },
    { title: 'Help a shy idea find its voice.', copy: 'What could make sharing feel safe, surprising, or wonderfully different?', mood: 'BRAVE LITTLE IDEAS' },
    { title: 'Turn a boring chore into a tiny adventure.', copy: 'The task stays. The feeling changes. Invent the transformation.', mood: 'JOYFUL REFRAMING' },
    { title: 'Design a welcome for someone from another planet.', copy: 'They do not know our customs. Make the first hello unforgettable.', mood: 'ALIEN EMPATHY' },
    { title: 'Make a rainy day the best day of the week.', copy: 'Build a reason to look forward to the clouds rolling in.', mood: 'WEATHER MAGIC' },
    { title: 'Give lost things a happy ending.', copy: 'Socks, keys, old ideas: what if nothing had to stay lost?', mood: 'KIND INVENTION' },
    { title: 'Make a place feel like it remembers you.', copy: 'Add a detail that says: you belong here, and we are glad you came.', mood: 'WORLD BUILDING' },
    { title: 'Help the moon with its night shift.', copy: 'It has been working for ages. What would make its job easier?', mood: 'COSMIC CARE' },
    { title: 'Make learning feel like discovering a secret.', copy: 'Hide a useful idea inside a game, a mystery, or a very odd object.', mood: 'CURIOUS MINDS' },
    { title: 'Invent a new way to say “I’m thinking of you.”', copy: 'No phone, no postcard. Make a little signal with a big heart.', mood: 'HUMAN CONNECTION' },
    { title: 'Help a city take a deep, happy breath.', copy: 'Imagine one small change that makes the whole place feel lighter.', mood: 'BETTER TOMORROWS' },
    { title: 'Make breakfast surprising again.', copy: 'Keep the toast. Change absolutely everything else.', mood: 'EVERYDAY ALCHEMY' },
    { title: 'Create a tiny celebration for no reason at all.', copy: 'Who gets invited? What happens? Why does it matter?', mood: 'UNNECESSARY JOY' },
    { title: 'Give a forgotten object a second life.', copy: 'Find a new purpose hiding inside an old, ordinary thing.', mood: 'FRESH PERSPECTIVES' }
  ];
  const decks = {
    wildcard: [
      ['a pocket-sized thunderstorm', 'weather'], ['the smell of cinnamon', 'senses'], ['a very patient octopus', 'nature'], ['a chorus of tiny bells', 'sound'],
      ['a map that redraws itself', 'wonder'], ['a sleepy houseplant', 'nature'], ['the color after a sunset', 'color'], ['a miniature parade', 'celebration'],
      ['a secret second moon', 'space'], ['a cloud that tells jokes', 'weather'], ['a borrowed memory', 'time'], ['an extremely polite robot', 'character'],
      ['the shape of a good idea', 'abstract'], ['a jar of friendly echoes', 'sound'], ['a door that opens sideways', 'wonder'], ['one very brave crumb', 'tiny things'],
      ['a tiny orchestra', 'sound'], ['a friendly shadow', 'character'], ['a reversible day', 'time'], ['the world’s softest ladder', 'materials'],
      ['a message from a whale', 'nature'], ['a pocket of zero gravity', 'space'], ['a library for smells', 'senses'], ['a tiny volcano of confetti', 'celebration']
    ].map(([label, note], id) => ({ id: `w${id}`, label, note })),
    audience: [
      ['a very sleepy dragon', 'needs a gentler morning'], ['someone who just moved here', 'doesn’t know anyone yet'], ['a kid who asks 400 questions', 'curiosity: unlimited'],
      ['the last lighthouse keeper', 'one light, a lot of sea'], ['a busy bee on a day off', 'trying to do absolutely nothing'], ['your future self', 'a little note from today'],
      ['a lonely mountain', 'it sees everyone pass by'], ['a nervous first-time inventor', 'they have one good try'], ['a family of tiny aliens', 'visiting for the weekend'],
      ['a grandparent learning a new trick', 'beginner energy'], ['the neighborhood cats', 'they have a meeting at 3'], ['a robot who loves poetry', 'but has never seen a flower'],
      ['someone who cannot sit still', 'every minute is an expedition'], ['a seed with big plans', 'it has not seen the sun yet'], ['a very old tree', 'it has heard everything'],
      ['a person who misses home', 'carry a little comfort'], ['a snail in a hurry', 'slow is a relative idea'], ['a classroom of future astronauts', 'launching imagination']
    ].map(([label, note], id) => ({ id: `a${id}`, label, note })),
    twist: [
      ['it only works at moonrise', 'timing is everything'], ['it must fit inside a teacup', 'tiny but mighty'], ['it can only communicate in song', 'la la la'],
      ['it gets better when shared', 'bring a friend'], ['it must use something you would recycle', 'give it another life'], ['it has one button and no instructions', 'figure it out together'],
      ['it changes color with your mood', 'wear your feelings'], ['it must be built from sounds', 'listen closely'], ['it works only when you whisper', 'shhh…'],
      ['it leaves a trail of tiny surprises', 'follow the clues'], ['it must help two opposites agree', 'unlikely friendship'], ['it has to be ready in ten seconds', 'quick-thinking magic'],
      ['it makes a boring thing feel rare', 'treasure the ordinary'], ['it must work without electricity', 'old-school clever'], ['it can only be used once a day', 'save the moment'],
      ['it grows a little each time you use it', 'keep it going'], ['it has to make a stranger smile', 'pass it on'], ['it is powered by a made-up word', 'say it out loud']
    ].map(([label, note], id) => ({ id: `t${id}`, label, note }))
  };

  const initial = { sparks: 0, rounds: 0, ideas: [], tried: [], feeling: '', sound: false };
  let saved = loadSave();
  let currentBrief = null;
  let currentCards = {};
  let selected = { wildcard: null, audience: null, twist: null };
  let activeFilter = 'all';
  let ink = '#24313a';
  let drawing = false;
  let sketchHasInk = false;
  let sketchHistory = [];
  let lastPoint = null;
  let audioContext = null;

  function loadSave() {
    try {
      const value = JSON.parse(localStorage.getItem(SAVE_KEY) || 'null');
      if (!value || value.version !== 1) return structuredClone(initial);
      const ideas = (Array.isArray(value.ideas) ? value.ideas : []).filter(idea => idea && typeof idea === 'object').slice(0, 60).map(idea => ({
        id: String(idea.id || ''), name: String(idea.name || 'Untitled wonder').slice(0, 80), pitch: String(idea.pitch || '').slice(0, 1000),
        prompt: String(idea.prompt || ''), mood: String(idea.mood || ''),
        ingredients: (Array.isArray(idea.ingredients) ? idea.ingredients : []).filter(item => typeof item === 'string').slice(0, 3),
        feeling: ['curious','surprising','useful'].includes(idea.feeling) ? idea.feeling : '',
        sparks: Math.max(0, Math.min(99, Number(idea.sparks) || 0)),
        createdAt: String(idea.createdAt || ''),
        sketch: typeof idea.sketch === 'string' && /^data:image\/jpeg;base64,[a-z0-9+/]+=*$/i.test(idea.sketch) ? idea.sketch : ''
      }));
      return {
        sparks: Math.max(0, Math.min(999999, Math.floor(Number(value.sparks) || 0))),
        rounds: Math.max(0, Math.min(999999, Math.floor(Number(value.rounds) || 0))),
        ideas, tried: (Array.isArray(value.tried) ? value.tried : []).filter(item => typeof item === 'string').slice(-500),
        feeling: '', sound: value.sound === true
      };
    } catch { return structuredClone(initial); }
  }
  function persist() {
    try { localStorage.setItem(SAVE_KEY, JSON.stringify({ ...saved, version: 1 })); return true; }
    catch { toast('Your browser storage is full. Export the Vault before planting more sketches.'); return false; }
  }
  function toast(message) {
    const el = $('toast'); el.textContent = message; el.classList.add('show');
    clearTimeout(toast.timer); toast.timer = setTimeout(() => el.classList.remove('show'), 2400);
  }
  function playChime() {
    if (!saved.sound) return;
    try {
      audioContext ||= new (window.AudioContext || window.webkitAudioContext)();
      const now = audioContext.currentTime;
      [523.25, 659.25, 783.99].forEach((hz, i) => {
        const osc = audioContext.createOscillator(), gain = audioContext.createGain();
        osc.type = 'sine'; osc.frequency.value = hz;
        gain.gain.setValueAtTime(.0001, now + i * .075);
        gain.gain.exponentialRampToValueAtTime(.065, now + i * .075 + .02);
        gain.gain.exponentialRampToValueAtTime(.0001, now + i * .075 + .17);
        osc.connect(gain); gain.connect(audioContext.destination); osc.start(now + i * .075); osc.stop(now + i * .075 + .19);
      });
    } catch { /* Audio is an optional little extra. */ }
  }
  function sparkBurst() {
    const button = $('saveIdeaButton').getBoundingClientRect();
    for (let i = 0; i < 12; i++) {
      const star = document.createElement('span'); star.className = 'spark-burst'; star.textContent = i % 3 ? '✦' : '✳';
      star.style.left = `${button.left + button.width * (.24 + Math.random() * .5)}px`;
      star.style.top = `${button.top + button.height * (.15 + Math.random() * .5)}px`;
      star.style.setProperty('--dx', `${(Math.random() - .5) * 130}px`); star.style.setProperty('--dy', `${-25 - Math.random() * 100}px`);
      document.body.appendChild(star); setTimeout(() => star.remove(), 1050);
    }
  }
  function updateHeader() {
    $('sparkTotal').textContent = saved.sparks;
    $('vaultCount').textContent = saved.ideas.length;
    $('roundNumber').textContent = `Round ${String(saved.rounds + 1).padStart(2, '0')}`;
    $('soundButton').setAttribute('aria-pressed', String(saved.sound));
    $('soundButton').setAttribute('aria-label', saved.sound ? 'Turn sound off' : 'Turn sound on');
    $('soundButton').title = saved.sound ? 'Sound on' : 'Sound off';
    $('soundButton').classList.toggle('sound-on', saved.sound);
  }
  function freshBrief() {
    const prior = currentBrief?.title;
    currentBrief = pick(promptDeck.filter(brief => brief.title !== prior));
    $('challengeTitle').textContent = currentBrief.title;
    $('challengeCopy').textContent = currentBrief.copy;
    $('moodLabel').textContent = currentBrief.mood;
    $('briefIndex').textContent = `OPEN PLAY · ${String(saved.rounds + 1).padStart(2, '0')}`;
    for (const key of Object.keys(decks)) {
      currentCards[key] = shuffle(decks[key]).slice(0, 3);
      selected[key] = currentCards[key][Math.floor(Math.random() * currentCards[key].length)];
      renderShelf(key);
    }
    $('ideaName').value = '';
    $('ideaPitch').value = '';
    $('pitchCount').textContent = '0 / 420';
    saved.feeling = '';
    document.querySelectorAll('.feeling-chip').forEach(button => button.classList.remove('selected'));
    clearCanvas(false); sketchHistory = [];
  }
  function renderShelf(key) {
    const container = $(`${key}Choices`);
    container.innerHTML = '';
    currentCards[key].forEach((card, index) => {
      const button = document.createElement('button'); button.type = 'button'; button.className = 'choice-card';
      button.setAttribute('aria-pressed', String(selected[key]?.id === card.id));
      button.setAttribute('aria-label', `${card.label}, ${card.note}`);
      button.innerHTML = `<span class="choice-label">${escapeHTML(card.label)}</span><span class="choice-note">${escapeHTML(card.note)}</span>`;
      button.addEventListener('click', () => {
        selected[key] = card; renderShelf(key); playChime();
        container.children[index]?.focus({ preventScroll: true });
      });
      container.appendChild(button);
    });
  }

  function selectScreen(name) {
    const workshop = name === 'workshop';
    $('workshopScreen').hidden = !workshop; $('vaultScreen').hidden = workshop;
    document.querySelectorAll('.nav-item').forEach(button => {
      const active = button.dataset.screen === name;
      button.classList.toggle('active', active);
      if (active) button.setAttribute('aria-current', 'page'); else button.removeAttribute('aria-current');
    });
    if (!workshop) renderVault();
    window.scrollTo({ top: 0, behavior: 'smooth' });
  }

  function createIdea() {
    const ingredient = selected.wildcard, audience = selected.audience, twist = selected.twist;
    if (!ingredient || !audience || !twist) { toast('Pick one idea seed from each shelf first.'); return; }
    const customName = $('ideaName').value.trim();
    const name = customName || `The ${titleCase(ingredient.label)} ${pick(['Machine', 'Club', 'Kit', 'Project', 'Express'])}`;
    const typedPitch = $('ideaPitch').value.trim();
    const pitch = typedPitch || `For ${audience.label}, ${name} uses ${ingredient.label} to answer “${currentBrief.title.replace(/[.!?]$/, '').toLowerCase()}” — and ${twist.label}.`;
    const wordCount = pitch.split(/\s+/).filter(Boolean).length;
    const signature = [currentBrief.title, ingredient.id, audience.id, twist.id].join('|');
    const freshCombination = !saved.tried.includes(signature);
    let reward = 2 + (freshCombination ? 2 : 0) + (wordCount >= 10 ? 1 : 0) + (sketchHasInk ? 1 : 0) + (saved.feeling ? 1 : 0);
    const idea = {
      id: `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`,
      name, pitch, prompt: currentBrief.title, mood: currentBrief.mood,
      ingredients: [ingredient.label, audience.label, twist.label],
      feeling: saved.feeling, sparks: reward, createdAt: new Date().toISOString(), sketch: sketchHasInk ? sketchCanvas().toDataURL('image/jpeg', .58) : ''
    };
    saved.ideas.unshift(idea); saved.ideas = saved.ideas.slice(0, 60);
    saved.tried.push(signature); if (saved.tried.length > 500) saved.tried.shift();
    saved.sparks += reward; saved.rounds++;
    persist(); updateHeader();
    toast(freshCombination ? `Idea planted · +${reward} sparks for trying a fresh mashup!` : `Idea planted · +${reward} sparks for making and exploring!`);
    playChime(); sparkBurst();
    freshBrief();
  }
  function titleCase(text) { return text.replace(/\b\w/g, char => char.toUpperCase()); }
  function sketchCanvas() { return $('sketchCanvas'); }
  function clearCanvas(keepHistory = true) {
    const canvas = sketchCanvas(); const context = canvas.getContext('2d');
    if (keepHistory && sketchHasInk) saveCanvasSnapshot();
    context.save(); context.fillStyle = '#ffffff'; context.fillRect(0, 0, canvas.width, canvas.height); context.restore();
    sketchHasInk = false; $('canvasPrompt').classList.remove('hidden');
  }
  function saveCanvasSnapshot() {
    try { sketchHistory.push(sketchCanvas().toDataURL()); if (sketchHistory.length > 12) sketchHistory.shift(); } catch { /* no-op */ }
  }
  function restoreCanvasSnapshot() {
    const canvas = sketchCanvas(), snapshot = sketchHistory.pop();
    if (!snapshot) { clearCanvas(false); return; }
    const image = new Image(); image.onload = () => { const ctx = canvas.getContext('2d'); ctx.clearRect(0, 0, canvas.width, canvas.height); ctx.drawImage(image, 0, 0); sketchHasInk = true; $('canvasPrompt').classList.add('hidden'); };
    image.src = snapshot;
  }
  function setupDrawing() {
    const canvas = sketchCanvas(), context = canvas.getContext('2d');
    context.fillStyle = '#fff'; context.fillRect(0, 0, canvas.width, canvas.height);
    context.lineCap = 'round'; context.lineJoin = 'round'; context.lineWidth = 5;
    const point = event => { const rect = canvas.getBoundingClientRect(); return { x: (event.clientX - rect.left) * canvas.width / rect.width, y: (event.clientY - rect.top) * canvas.height / rect.height }; };
    canvas.addEventListener('pointerdown', event => {
      event.preventDefault(); drawing = true; lastPoint = point(event); saveCanvasSnapshot(); canvas.setPointerCapture(event.pointerId);
      context.beginPath(); context.fillStyle = ink; context.arc(lastPoint.x, lastPoint.y, 2.8, 0, Math.PI * 2); context.fill();
      sketchHasInk = true; $('canvasPrompt').classList.add('hidden');
    });
    canvas.addEventListener('pointermove', event => {
      if (!drawing || !lastPoint) return;
      const next = point(event); context.beginPath(); context.strokeStyle = ink; context.lineWidth = 5;
      context.moveTo(lastPoint.x, lastPoint.y); context.lineTo(next.x, next.y); context.stroke(); lastPoint = next;
    });
    const finish = () => { drawing = false; lastPoint = null; };
    canvas.addEventListener('pointerup', finish); canvas.addEventListener('pointercancel', finish); canvas.addEventListener('lostpointercapture', finish);
    $('undoSketch').addEventListener('click', restoreCanvasSnapshot);
    $('clearSketch').addEventListener('click', () => { clearCanvas(true); sketchHistory = []; });
    document.querySelectorAll('.color-dot').forEach(button => button.addEventListener('click', () => {
      ink = button.dataset.color; document.querySelectorAll('.color-dot').forEach(dot => dot.classList.toggle('selected', dot === button));
    }));
  }

  function renderVault() {
    const grid = $('vaultGrid');
    const ideas = saved.ideas.filter(idea => activeFilter === 'all' || idea.feeling === activeFilter);
    $('vaultTotal').textContent = saved.ideas.length;
    $('emptyVault').hidden = saved.ideas.length > 0;
    grid.innerHTML = '';
    ideas.forEach((idea, index) => {
      const card = document.createElement('article'); card.className = 'idea-card'; card.style.animationDelay = `${Math.min(index * 35, 250)}ms`;
      const date = new Date(idea.createdAt); const dateText = Number.isNaN(date.getTime()) ? 'JUST PLANTED' : date.toLocaleDateString(undefined, { month: 'short', day: 'numeric' }).toUpperCase();
      const badge = idea.feeling ? idea.feeling.toUpperCase() : 'A NEW POSSIBILITY';
      card.innerHTML = `<div class="idea-card-top"><span class="idea-card-badge">${escapeHTML(badge)}</span><span class="idea-card-date">${escapeHTML(dateText)}</span></div>${idea.sketch ? `<img class="idea-card-sketch" src="${idea.sketch}" alt="Sketch for ${escapeHTML(idea.name)}">` : ''}<div class="idea-card-body"><h3>${escapeHTML(idea.name)}</h3><p>${escapeHTML(idea.pitch)}</p><div class="idea-ingredients">${(idea.ingredients || []).map(item => `<span>${escapeHTML(item)}</span>`).join('')}</div><div class="idea-card-foot"><span class="feeling-tag">${idea.sparks || 0} ✳ sparks · kept by you</span><div class="card-actions"><button data-action="copy" data-id="${escapeHTML(idea.id)}" aria-label="Copy ${escapeHTML(idea.name)}">Copy</button><button data-action="delete" data-id="${escapeHTML(idea.id)}" aria-label="Delete ${escapeHTML(idea.name)}">Remove</button></div></div></div>`;
      grid.appendChild(card);
    });
    $('vaultCount').textContent = saved.ideas.length;
  }
  function exportIdeas() {
    if (!saved.ideas.length) { toast('Your Vault is empty — plant an idea first.'); return; }
    const content = JSON.stringify({ title: 'Wonderworks Idea Vault', exportedAt: new Date().toISOString(), ideas: saved.ideas }, null, 2);
    const link = document.createElement('a'); link.href = URL.createObjectURL(new Blob([content], { type: 'application/json' }));
    link.download = `wonderworks-ideas-${new Date().toISOString().slice(0, 10)}.json`; link.click(); URL.revokeObjectURL(link.href);
    toast('Your ideas are packed up and ready to go.');
  }
  $('vaultGrid').addEventListener('click', async event => {
    const button = event.target.closest('button[data-action]'); if (!button) return;
    const idea = saved.ideas.find(item => item.id === button.dataset.id); if (!idea) return;
    if (button.dataset.action === 'delete') {
      saved.ideas = saved.ideas.filter(item => item.id !== idea.id); persist(); updateHeader(); renderVault(); toast('Idea gently tucked away.');
    } else if (button.dataset.action === 'copy') {
      const text = `${idea.name}\n${idea.pitch}\n\nIngredients: ${(idea.ingredients || []).join(' · ')}`;
      try { await navigator.clipboard.writeText(text); toast('Idea copied to your clipboard.'); }
      catch { toast('Copy is unavailable here — use Export my ideas instead.'); }
    }
  });
  document.querySelectorAll('.filter-button').forEach(button => button.addEventListener('click', () => {
    activeFilter = button.dataset.filter;
    document.querySelectorAll('.filter-button').forEach(item => { const active = item === button; item.classList.toggle('selected', active); item.setAttribute('aria-selected', String(active)); });
    renderVault();
  }));

  document.querySelectorAll('.nav-item').forEach(button => button.addEventListener('click', () => selectScreen(button.dataset.screen)));
  $('remixButton').addEventListener('click', () => {
    const hasDraft = $('ideaName').value.trim() || $('ideaPitch').value.trim() || sketchHasInk;
    if (hasDraft && !window.confirm('You have an unplanted idea. Start a fresh brief and clear this draft?')) return;
    freshBrief(); toast('Fresh ingredients! Follow whichever one catches your eye.');
  });
  $('saveIdeaButton').addEventListener('click', createIdea);
  $('ideaPitch').addEventListener('input', () => { $('pitchCount').textContent = `${$('ideaPitch').value.length} / 420`; });
  document.querySelectorAll('.feeling-chip').forEach(button => button.addEventListener('click', () => {
    const active = button.classList.contains('selected'); document.querySelectorAll('.feeling-chip').forEach(item => item.classList.remove('selected'));
    saved.feeling = active ? '' : button.dataset.feeling; if (!active) button.classList.add('selected');
  }));
  $('helpButton').addEventListener('click', () => $('helpDialog').showModal());
  $('soundButton').addEventListener('click', () => { saved.sound = !saved.sound; persist(); updateHeader(); if (saved.sound) playChime(); toast(saved.sound ? 'Tiny celebratory sounds are on.' : 'Sound is off. Peace and quiet, too.'); });
  $('firstIdeaButton').addEventListener('click', () => selectScreen('workshop'));
  $('exportButton').addEventListener('click', exportIdeas);
  $('privacyButton').addEventListener('click', () => toast('Nothing leaves this device. Your Vault is stored in local app storage.'));
  window.addEventListener('keydown', event => { if ((event.ctrlKey || event.metaKey) && event.key === 'Enter' && !$('workshopScreen').hidden) createIdea(); });

  setupDrawing(); updateHeader(); freshBrief();
})();
