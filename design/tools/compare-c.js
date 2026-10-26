// Side-by-side check of the firmware's C port against this renderer.
// Builds firmware/test/host_render, plays the same demo in both, and writes
// out/compare-<mode>.png: JS frames on the left, C frames on the right.
//
//   node tools/compare-c.js [hex|b70] [t1,t2,...] [scale]
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const B = require('../babel-screen.js');
const { encodeIndexed, scale } = require('./png.js');

const mode = process.argv[2] || 'hex';
const times = (process.argv[3] || '1,4,6.5,9,11.2,14,17,20').split(',').map(Number).sort((a, b) => a - b);
const S = Number(process.argv[4] || 2);
const root = path.join(__dirname, '..', '..');
const testDir = path.join(root, 'firmware', 'test');
const outDir = path.join(__dirname, '..', 'out');
fs.mkdirSync(outDir, { recursive: true });

execFileSync('make', ['-s', '-C', testDir], { stdio: 'inherit' });
const raw = path.join(outDir, `.c-frames-${mode}.raw`);
execFileSync(path.join(testDir, 'host_render'), [mode, times.join(','), raw], { stdio: 'inherit' });
const cbuf = fs.readFileSync(raw);
fs.unlinkSync(raw);

const sc = new B.Scene({ mode, seed: 3 });
sc.director = B.demoDirector(B.DEMO_SCRIPT);
const fb = B.createFrame();
const N = B.SW * B.SH, dt = 1 / 60;
const pairs = [];
let t = 0;
times.forEach((target, i) => {
  while (t + dt / 2 < target) { sc.update(dt); t += dt; }
  sc.render(fb);
  pairs.push([fb.slice(), cbuf.subarray(i * N, (i + 1) * N)]);
});

const W = B.SW * S, H = B.SH * S, G = 4;
const SWt = 2 * W + 3 * G, SHt = pairs.length * H + (pairs.length + 1) * G;
const sheet = new Uint8Array(SWt * SHt).fill(1);
pairs.forEach(([js, c], i) => {
  let diff = 0;
  for (let k = 0; k < N; k++) if (js[k] !== c[k]) diff++;
  console.log(`t=${times[i]}  differing pixels: ${diff} (${(100 * diff / N).toFixed(1)}%)`);
  [js, c].forEach((px, j) => {
    const big = scale(px, B.SW, B.SH, S);
    const ox = G + j * (W + G), oy = G + i * (H + G);
    for (let y = 0; y < H; y++) sheet.set(big.subarray(y * W, y * W + W), (oy + y) * SWt + ox);
  });
});
const name = path.join(outDir, `compare-${mode}.png`);
fs.writeFileSync(name, encodeIndexed(SWt, SHt, sheet, B.PALETTE.map(p => p.rgb)));
console.log('wrote', path.relative(process.cwd(), name));
