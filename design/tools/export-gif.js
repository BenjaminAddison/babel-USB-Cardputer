// Export a seamless GIF of one host session: root -> "Hello" -> read -> "Help" -> read -> root.
//   node tools/export-gif.js [hex|b70] [scale] [fps]
const fs = require('fs');
const path = require('path');
const B = require('../babel-screen.js');
const { GifWriter } = require('./gif.js');
const { scale } = require('./png.js');

const mode = process.argv[2] || 'hex';
const S = Number(process.argv[3] || 1);
const fps = Number(process.argv[4] || 25);
const LOOP = [
  ['goto', 'Hello', 0.32], ['wait', 0.5],
  ['open'], ['wait', 2.2], ['close'], ['wait', 0.5],
  ['up'], ['wait', 0.45],
  ['goto', 'Help', 0.3], ['wait', 0.4],
  ['open'], ['wait', 1.5], ['close'], ['wait', 0.5],
  ['root'], ['wait', 1.0],
];
const DT = 0.01;

function run(periodic) {
  const sc = new B.Scene({ mode, seed: 3 });
  sc.autoTilt = false;
  sc.connect();
  for (let i = 0; i < 300; i++) sc.update(DT);           // settle: lamps lit, toast gone
  const t0 = sc.t;
  if (periodic) {
    const L = periodic.L, w = 2 * Math.PI / L;
    sc.tiltFn = t => ({ x: 4.5 * Math.sin((t - t0) * w), y: 2.2 * Math.sin((t - t0) * 2 * w + 1.1), roll: 0.03 * Math.sin((t - t0) * w + 0.6) });
    sc.bobFreq = w * Math.max(1, Math.round(L / 4.65));
  }
  sc.director = B.demoDirector(LOOP, { loop: false });
  return { sc, t0 };
}

// pass 1: how long is one session?
let { sc, t0 } = run(null);
while (!(sc.director.done && !sc.act && !sc.queue.length && !sc.flyers.length)) sc.update(DT);
const L = sc.t - t0;

// pass 2: render with tilt and bob that repeat exactly once per loop
({ sc, t0 } = run({ L }));
const frames = Math.round(L * fps);
const delay = Math.round(100 / fps);
const gif = new GifWriter(B.SW * S, B.SH * S, B.PALETTE.map(p => p.rgb));
const fb = B.createFrame();
for (let i = 0; i < frames; i++) {
  const target = t0 + i / fps;
  while (sc.t + DT / 2 < target) sc.update(DT);
  sc.render(fb);
  gif.addFrame(S === 1 ? fb : scale(fb, B.SW, B.SH, S), delay);
}
const out = path.join(__dirname, '..', 'out', `babel-screen-${mode}${S > 1 ? '@' + S + 'x' : ''}.gif`);
const buf = gif.end();
fs.writeFileSync(out, buf);
console.log(`wrote ${out}  ${frames} frames, ${L.toFixed(2)} s loop, ${(buf.length / 1024).toFixed(0)} KB`);
