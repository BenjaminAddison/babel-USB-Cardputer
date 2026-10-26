// Minimal animated GIF89a writer for palette frames (no dependencies).
// Frames after the first are written as transparent deltas over the previous one.

function lzw(indices, minCodeSize, out) {
  // Same scheme as omggif's encoder: bump the code size before assigning a
  // code that no longer fits, clear the table at 4096.
  out.push(minCodeSize);
  let sub = out.length; out.push(0);
  const clear = 1 << minCodeSize, eoi = clear + 1;
  let next = eoi + 1, size = minCodeSize + 1, cur = 0, shift = 0;
  const flush = (min) => {
    while (shift >= min) {
      out.push(cur & 255); cur >>>= 8; shift -= 8;
      if (out.length === sub + 256) { out[sub] = 255; sub = out.length; out.push(0); }
    }
  };
  const emit = (c) => { cur |= c << shift; shift += size; flush(8); };
  let table = new Map();
  emit(clear);
  let prefix = indices[0];
  for (let i = 1; i < indices.length; i++) {
    const k = indices[i];
    const key = (prefix << 8) | k;
    const found = table.get(key);
    if (found !== undefined) { prefix = found; continue; }
    emit(prefix);
    if (next === 4096) { emit(clear); next = eoi + 1; size = minCodeSize + 1; table = new Map(); }
    else { if (next >= (1 << size)) size++; table.set(key, next++); }
    prefix = k;
  }
  emit(prefix); emit(eoi); flush(1);
  if (shift > 0) { out.push(cur & 255); if (out.length === sub + 256) { out[sub] = 255; sub = out.length; out.push(0); } }
  out[sub] = out.length - sub - 1;
  if (out[sub] > 0) out.push(0); else out[sub] = 0;
}

class GifWriter {
  constructor(width, height, palette16) {
    this.w = width; this.h = height; this.prev = null;
    this.bytes = [];
    const b = this.bytes;
    const u16 = v => { b.push(v & 255, (v >> 8) & 255); };
    b.push(...Buffer.from('GIF89a', 'ascii'));
    u16(width); u16(height);
    b.push(0xf4, 0, 0);                      // 32-entry global table
    for (let i = 0; i < 32; i++) {
      const c = palette16[i] || [0, 0, 0];
      b.push(c[0], c[1], c[2]);
    }
    b.push(0x21, 0xff, 0x0b, ...Buffer.from('NETSCAPE2.0', 'ascii'), 3, 1, 0, 0, 0);   // loop forever
  }
  addFrame(px, delayCs) {
    const { w, h } = this, b = this.bytes;
    let x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1, data, transparent = false;
    if (this.prev) {
      x0 = w; y0 = h; x1 = -1; y1 = -1;
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        if (px[y * w + x] !== this.prev[y * w + x]) {
          if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
      }
      if (x1 < 0) { x0 = 0; y0 = 0; x1 = 0; y1 = 0; }
      transparent = true;
    }
    const fw = x1 - x0 + 1, fh = y1 - y0 + 1;
    data = new Uint8Array(fw * fh);
    for (let y = 0; y < fh; y++) for (let x = 0; x < fw; x++) {
      const i = (y + y0) * w + (x + x0);
      data[y * fw + x] = transparent && this.prev[i] === px[i] ? 16 : px[i];
    }
    b.push(0x21, 0xf9, 4, (1 << 2) | (transparent ? 1 : 0), delayCs & 255, (delayCs >> 8) & 255, 16, 0);
    b.push(0x2c, x0 & 255, x0 >> 8, y0 & 255, y0 >> 8, fw & 255, fw >> 8, fh & 255, fh >> 8, 0);
    lzw(data, 5, b);
    this.prev = px.slice();
  }
  end() { this.bytes.push(0x3b); return Buffer.from(this.bytes); }
}
module.exports = { GifWriter };
