const fs = require('fs');
let t0 = Date.now();
const nowUs = () => BigInt((Date.now() - t0) * 1000);
const env = {
  js_now_us: nowUs,
  js_present: () => {},
  js_poll_event: () => 0,
  js_wallclock: () => {},
  js_set_title: () => {},
  js_beep: () => {},
  js_log: () => {},
  js_request_quit: () => {},
};
const runMs = parseInt(process.argv[2] || '4200', 10);
const outPath = process.argv[3] || '/tmp/wasm_shot.ppm';
WebAssembly.instantiate(fs.readFileSync('app/aether.wasm'), { env }).then(({ instance }) => {
  const e = instance.exports;
  e.aether_boot();
  const W = e.aether_screen_w(), H = e.aether_screen_h(), vram = e.aether_vram() >>> 0;
  const deadline = Date.now() + runMs;
  let n = 0;
  while (Date.now() < deadline) { e.aether_frame(); n++; }
  const px = new Uint32Array(e.memory.buffer, vram, W * H);
  const out = Buffer.alloc(W * H * 3);
  for (let i = 0; i < W * H; i++) {
    const c = px[i];
    out[i * 3] = (c >> 16) & 255; out[i * 3 + 1] = (c >> 8) & 255; out[i * 3 + 2] = c & 255;
  }
  fs.writeFileSync(outPath, Buffer.concat([Buffer.from(`P6\n${W} ${H}\n255\n`, 'ascii'), out]));
  console.log('frames:', n, 'running:', e.aether_running(), '->', outPath);
});
