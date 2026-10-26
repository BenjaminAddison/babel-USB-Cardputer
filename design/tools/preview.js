// Render chosen moments of the demo loop to PNG for review.
//   node tools/preview.js [hex|b70] [t1,t2,...] [scale] [sheetCols]
const fs = require('fs');
const path = require('path');
const B = require('../babel-screen.js');
const { encodeIndexed, scale } = require('./png.js');

const mode = process.argv[2] || 'hex';
const times = (process.argv[3] || '1,4,6.5,9,11.2,14,17,20').split(',').map(Number);
const S = Number(process.argv[4] || 3);
const cols = Number(process.argv[5] || 2);
const outDir = path.join(__dirname, '..', 'out');
fs.mkdirSync(outDir, { recursive: true });

const sc = new B.Scene({ mode, seed: 3 });
sc.director = B.demoDirector(B.DEMO_SCRIPT);
const fb = B.createFrame();
const pal = B.PALETTE.map(p => p.rgb);
const dt = 1 / 60;
const frames = [];
let t = 0;
for (const target of times.slice().sort((a, b) => a - b)) {
  while (t + dt / 2 < target) { sc.update(dt); t += dt; }
  sc.render(fb);
  frames.push({ t: target, px: fb.slice() });
}

// contact sheet with a 4px gutter
const W = B.SW * S, H = B.SH * S, G = 4 * S / 3 | 0;
const rows = Math.ceil(frames.length / cols);
const SWt = cols * W + (cols + 1) * G, SHt = rows * H + (rows + 1) * G;
const sheet = new Uint8Array(SWt * SHt).fill(1);
frames.forEach((f, i) => {
  const big = scale(f.px, B.SW, B.SH, S);
  const ox = G + (i % cols) * (W + G), oy = G + ((i / cols) | 0) * (H + G);
  for (let y = 0; y < H; y++) sheet.set(big.subarray(y * W, y * W + W), (oy + y) * SWt + ox);
});
const name = path.join(outDir, `sheet-${mode}.png`);
fs.writeFileSync(name, encodeIndexed(SWt, SHt, sheet, pal));
console.log('wrote', name, frames.map(f => f.t).join(', '));
console.log('events:', sc.events.map(e => `${e.t.toFixed(2)} ${e.op} ${e.detail}`).join('\n  '));
