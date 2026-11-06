'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

class FakeClassList {
  constructor() { this.values = new Set(); }
  add(name) { this.values.add(name); }
  remove(name) { this.values.delete(name); }
  contains(name) { return this.values.has(name); }
  toggle(name, force) { const enabled = force === undefined ? !this.values.has(name) : Boolean(force); enabled ? this.add(name) : this.remove(name); return enabled; }
}
class FakeElement {
  constructor(id = '') {
    Object.assign(this, { id, children: [], dataset: {}, style: { setProperty(name, value) { this[name] = value; } }, classList: new FakeClassList(), attributes: {}, listeners: {},
      value: '', textContent: '', _innerHTML: '', hidden: false, width: id === 'sketchCanvas' ? 820 : 300, height: id === 'sketchCanvas' ? 240 : 80 });
  }
  addEventListener(type, callback) { (this.listeners[type] ||= []).push(callback); }
  fire(type, event = {}) { for (const callback of this.listeners[type] || []) callback({ target: this, preventDefault() {}, ...event }); }
  appendChild(element) { this.children.push(element); element.parentElement = this; return element; }
  remove() { if (this.parentElement) this.parentElement.children = this.parentElement.children.filter(item => item !== this); }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  removeAttribute(name) { delete this.attributes[name]; }
  set innerHTML(value) { this._innerHTML = String(value); if (!value) this.children = []; }
  get innerHTML() { return this._innerHTML; }
  getBoundingClientRect() { return { left: 0, top: 0, width: 400, height: 100 }; }
  getContext() { return context; }
  toDataURL() { return 'data:image/jpeg;base64,dGVzdA=='; }
  setPointerCapture() {}
  focus() {}
  showModal() { this.open = true; }
  close() { this.open = false; }
  closest() { return null; }
  querySelector() { return new FakeElement(); }
}
const noop = () => {};
const context = new Proxy({ createLinearGradient: () => ({ addColorStop: noop }), createRadialGradient: () => ({ addColorStop: noop }) }, {
  get: (target, key) => target[key] || noop,
  set: (target, key, value) => { target[key] = value; return true; }
});
const ids = {};
const idList = ['sparkTotal','vaultCount','roundNumber','soundButton','challengeTitle','challengeCopy','moodLabel','briefIndex','ideaName','ideaPitch','pitchCount','canvasPrompt','sketchCanvas','undoSketch','clearSketch','workshopScreen','vaultScreen','vaultGrid','emptyVault','vaultTotal','toast','saveIdeaButton','helpDialog','helpButton','remixButton','firstIdeaButton','exportButton','privacyButton'];
for (const id of idList) ids[id] = new FakeElement(id);
for (const id of ['wildcardChoices','audienceChoices','twistChoices']) ids[id] = new FakeElement(id);
ids.saveIdeaButton.getBoundingClientRect = () => ({ left: 30, top: 30, width: 160, height: 40 });
const selections = {
  '.nav-item': ['workshop','vault'].map(screen => { const el = new FakeElement(); el.dataset.screen = screen; return el; }),
  '.feeling-chip': ['curious','surprising','useful'].map(feeling => { const el = new FakeElement(); el.dataset.feeling = feeling; return el; }),
  '.filter-button': ['all','curious','surprising','useful'].map(filter => { const el = new FakeElement(); el.dataset.filter = filter; return el; }),
  '.color-dot': ['#24313a','#ff715b','#5f82e6','#e4ad33'].map(color => { const el = new FakeElement(); el.dataset.color = color; return el; })
};
const document = {
  body: new FakeElement('body'),
  getElementById: id => ids[id] ||= new FakeElement(id),
  querySelectorAll: selector => selections[selector] || [],
  createElement: tag => new FakeElement(tag),
  addEventListener: noop
};
const storage = new Map();
const localStorage = { getItem: key => storage.get(key) || null, setItem: (key, value) => storage.set(key, String(value)) };
const window = { localStorage, scrollTo: noop, addEventListener: noop };
const source = fs.readFileSync('game.js', 'utf8').replace('  setupDrawing(); updateHeader(); freshBrief();',
  '  window.__test = { saved, selected, currentCards, freshBrief, createIdea, renderVault, selectScreen };\n  setupDrawing(); updateHeader(); freshBrief();');
vm.runInNewContext(source, { window, document, localStorage, structuredClone, Math, Date, JSON, Blob, URL, navigator: {}, Image: class {}, setTimeout: () => 1, clearTimeout: noop, console });

const game = window.__test;
assert.ok(game.selected.wildcard && game.selected.audience && game.selected.twist, 'A round starts with all three ingredient shelves ready.');
const originalBrief = ids.challengeTitle.textContent;
ids.ideaName.value = 'The Moonbeam Umbrella';
ids.ideaPitch.value = 'A tiny weather friend turns waiting for the bus into a shared story with warm light and surprise.';
ids.sketchCanvas.fire('pointerdown', { pointerId: 1, clientX: 40, clientY: 30 });
ids.sketchCanvas.fire('pointermove', { pointerId: 1, clientX: 90, clientY: 55 });
ids.sketchCanvas.fire('pointerup', { pointerId: 1 });
selections['.feeling-chip'][0].fire('click');
ids.saveIdeaButton.fire('click');
assert.equal(game.saved.ideas.length, 1, 'Planting saves a completed invention.');
assert.equal(game.saved.ideas[0].name, 'The Moonbeam Umbrella');
assert.ok(game.saved.ideas[0].sketch.startsWith('data:image/jpeg'), 'The optional prototype sketch is stored with its idea.');
assert.ok(game.saved.sparks > 0, 'Trying a new mashup earns sparks.');
assert.equal(game.saved.rounds, 1);
assert.ok(storage.has('wonderworks.arcade.save.v1'), 'The local save is persisted.');
assert.notEqual(ids.challengeTitle.textContent, originalBrief, 'Planting advances to a new brief.');
game.selectScreen('vault');
assert.equal(ids.vaultScreen.hidden, false, 'The Idea Vault opens.');
game.renderVault();
console.log('Wonderworks smoke test passed: ingredient round, sketch input, reflection, spark reward, local save, next brief, and Idea Vault.');
