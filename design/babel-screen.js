/*
 * babel-screen.js  ·  "The Air Shaft"
 * ---------------------------------------------------------------------------
 * Screen concept for a USB-of-Babel firmware on the M5Stack Cardputer ADV.
 *
 *   "The universe (which others call the Library) is composed of an
 *    indefinite, perhaps infinite number of hexagonal galleries, with vast
 *    air shafts between, surrounded by very low railings."     (Borges)
 *
 * You are looking straight down one of those air shafts. Every floor is a
 * folder. The name of the next floor flickers through every possibility
 * until the host picks one; as you drop through it, the name falls into the
 * path bar. At the bottom of the shaft, always just out of reach, floats the
 * book: the one `file` in every folder, whose contents are the path you took.
 *
 * Rendering model
 *   240x135, one byte per pixel holding a palette index 0..15. On the device
 *   that is a 4bpp M5Canvas (16.2 KB) pushed through a 16-entry RGB565
 *   palette, so every pixel on screen is literally one hex digit. All shading
 *   is ordered (Bayer 4x4) dithering along per-material colour ramps, which
 *   ports to C++ unchanged. No DOM, no allocation in the render loop: the same
 *   file drives the browser preview and the Node GIF exporter.
 *
 * Scene API (what the firmware's MTP handlers would call)
 *   connect()        MTP OpenSession
 *   disconnect()     session closed / cable pulled
 *   enter(comp)      GetObjectHandles(parent) with a new parent
 *   up()             host went back one folder
 *   root()           GetObjectHandles(0xFFFFFFFF)
 *   open() / close() GetObject on the `file` handle
 *   navigateTo(path) catalog lookup typed on the Cardputer keyboard
 */
(function (root) {
  'use strict';

  const SW = 240, SH = 135;

  /* ================================================================ palette */
  // A lamplit library falling into darkness, plus one cold "data" accent.
  // Values are snapped to RGB565 below so the preview matches the panel.
  const PALETTE_SRC = [
    ['VOID',    '#05060b', 'Bottomless dark and fog'],
    ['INK',     '#0e101a', 'Catalog card, deep shadow'],
    ['SHADE',   '#1b1e2c', 'Shadowed shelves'],
    ['DUSK',    '#34384e', 'Dim text, separators'],
    ['UMBER',   '#3c2619', 'Wood in shadow'],
    ['WOOD',    '#6f4628', 'Shelf boards, railing'],
    ['OXBLOOD', '#8b2d2a', 'Book cloth, catalog rule'],
    ['MOSS',    '#3e6b45', 'Book cloth'],
    ['INDIGO',  '#344b7e', 'Book cloth'],
    ['OCHRE',   '#ba8b36', 'Gilt titles, wordmark'],
    ['VELLUM',  '#e9dbb5', 'Pages, file contents'],
    ['LAMP',    '#ffb94e', 'The spherical lamps'],
    ['EMBER',   '#c26b23', 'Lamplit wood, glow'],
    ['CYAN',    '#5fe3cf', 'Folder names, the path'],
    ['WHITE',   '#fff6e3', 'Lock flash, lamp cores'],
    ['CRIMSON', '#ff4e5f', 'Activity LED'],
  ];
  const VOID = 0, INK = 1, SHADE = 2, DUSK = 3, UMBER = 4, WOOD = 5, OXBLOOD = 6, MOSS = 7,
        INDIGO = 8, OCHRE = 9, VELLUM = 10, LAMP = 11, EMBER = 12, CYAN = 13, WHITE = 14, CRIMSON = 15;

  function to565(hex) {
    const n = parseInt(hex.slice(1), 16);
    const r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
    return (Math.round(r * 31 / 255) << 11) | (Math.round(g * 63 / 255) << 5) | Math.round(b * 31 / 255);
  }
  function from565(c) {
    const r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)];
  }
  const PALETTE = PALETTE_SRC.map(([name, hex, role], i) => {
    const c565 = to565(hex);
    const rgb = from565(c565);
    return { index: i, name, role, hex, rgb565: c565, rgb,
      css: '#' + rgb.map(v => v.toString(16).padStart(2, '0')).join('') };
  });
  // One step darker for every colour: used to sink the shaft behind the open book.
  const DARKER = [VOID, VOID, INK, SHADE, INK, UMBER, UMBER, SHADE, SHADE, WOOD, DUSK, EMBER, UMBER, DUSK, VELLUM, OXBLOOD];

  /* ========================================================= dither, noise */
  const BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];
  const bayer = (x, y) => (BAYER[((y & 3) << 2) | (x & 3)] + 0.5) / 16;

  function rng(seed) {
    let s = (seed >>> 0) || 0x9e3779b9;
    return function () {
      s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0;
      return s / 4294967296;
    };
  }
  function hash(a, b) {
    let h = Math.imul(a | 0, 0x27d4eb2d) ^ Math.imul((b | 0) + 0x632be5ab, 0x165667b1);
    h = Math.imul(h ^ (h >>> 15), 0x85ebca6b);
    h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
    return (h ^ (h >>> 16)) >>> 0;
  }
  const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
  const lerp = (a, b, t) => a + (b - a) * t;
  const easeInOut = t => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
  const easeOut = t => 1 - Math.pow(1 - t, 3);
  const easeIn = t => t * t;

  /* ================================================================= fonts */
  // 5x7 body font on a 6x9 cell (row 7 holds descenders). Full printable ASCII
  // plus the few symbols the screen needs.
  const FONT5_SRC = {
    ' ': '.....|.....|.....|.....|.....|.....|.....',
    '!': '..#..|..#..|..#..|..#..|..#..|.....|..#..',
    '"': '.#.#.|.#.#.|.#.#.|.....|.....|.....|.....',
    '#': '.#.#.|.#.#.|#####|.#.#.|#####|.#.#.|.#.#.',
    '$': '..#..|.####|#.#..|.###.|..#.#|####.|..#..',
    '%': '##...|##..#|...#.|..#..|.#...|#..##|...##',
    '&': '.##..|#..#.|#.#..|.#...|#.#.#|#..#.|.##.#',
    "'": '..#..|..#..|..#..|.....|.....|.....|.....',
    '(': '...#.|..#..|.#...|.#...|.#...|..#..|...#.',
    ')': '.#...|..#..|...#.|...#.|...#.|..#..|.#...',
    '*': '.....|..#..|#.#.#|.###.|#.#.#|..#..|.....',
    '+': '.....|..#..|..#..|#####|..#..|..#..|.....',
    ',': '.....|.....|.....|.....|.##..|..#..|.#...',
    '-': '.....|.....|.....|#####|.....|.....|.....',
    '.': '.....|.....|.....|.....|.....|.##..|.##..',
    '/': '.....|....#|...#.|..#..|.#...|#....|.....',
    '0': '.###.|#...#|#..##|#.#.#|##..#|#...#|.###.',
    '1': '..#..|.##..|..#..|..#..|..#..|..#..|.###.',
    '2': '.###.|#...#|....#|...#.|..#..|.#...|#####',
    '3': '#####|...#.|..#..|...#.|....#|#...#|.###.',
    '4': '...#.|..##.|.#.#.|#..#.|#####|...#.|...#.',
    '5': '#####|#....|####.|....#|....#|#...#|.###.',
    '6': '..##.|.#...|#....|####.|#...#|#...#|.###.',
    '7': '#####|....#|...#.|..#..|.#...|.#...|.#...',
    '8': '.###.|#...#|#...#|.###.|#...#|#...#|.###.',
    '9': '.###.|#...#|#...#|.####|....#|...#.|.##..',
    ':': '.....|.##..|.##..|.....|.##..|.##..|.....',
    ';': '.....|.##..|.##..|.....|.##..|..#..|.#...',
    '<': '...#.|..#..|.#...|#....|.#...|..#..|...#.',
    '=': '.....|.....|#####|.....|#####|.....|.....',
    '>': '.#...|..#..|...#.|....#|...#.|..#..|.#...',
    '?': '.###.|#...#|....#|...#.|..#..|.....|..#..',
    '@': '.###.|#...#|....#|.##.#|#.#.#|#.#.#|.###.',
    'A': '.###.|#...#|#...#|#####|#...#|#...#|#...#',
    'B': '####.|#...#|#...#|####.|#...#|#...#|####.',
    'C': '.###.|#...#|#....|#....|#....|#...#|.###.',
    'D': '###..|#..#.|#...#|#...#|#...#|#..#.|###..',
    'E': '#####|#....|#....|####.|#....|#....|#####',
    'F': '#####|#....|#....|####.|#....|#....|#....',
    'G': '.###.|#...#|#....|#.###|#...#|#...#|.####',
    'H': '#...#|#...#|#...#|#####|#...#|#...#|#...#',
    'I': '.###.|..#..|..#..|..#..|..#..|..#..|.###.',
    'J': '..###|...#.|...#.|...#.|...#.|#..#.|.##..',
    'K': '#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#',
    'L': '#....|#....|#....|#....|#....|#....|#####',
    'M': '#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#',
    'N': '#...#|#...#|##..#|#.#.#|#..##|#...#|#...#',
    'O': '.###.|#...#|#...#|#...#|#...#|#...#|.###.',
    'P': '####.|#...#|#...#|####.|#....|#....|#....',
    'Q': '.###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#',
    'R': '####.|#...#|#...#|####.|#.#..|#..#.|#...#',
    'S': '.####|#....|#....|.###.|....#|....#|####.',
    'T': '#####|..#..|..#..|..#..|..#..|..#..|..#..',
    'U': '#...#|#...#|#...#|#...#|#...#|#...#|.###.',
    'V': '#...#|#...#|#...#|#...#|#...#|.#.#.|..#..',
    'W': '#...#|#...#|#...#|#.#.#|#.#.#|#.#.#|.#.#.',
    'X': '#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#',
    'Y': '#...#|#...#|#...#|.#.#.|..#..|..#..|..#..',
    'Z': '#####|....#|...#.|..#..|.#...|#....|#####',
    '[': '.###.|.#...|.#...|.#...|.#...|.#...|.###.',
    '\\': '.....|#....|.#...|..#..|...#.|....#|.....',
    ']': '.###.|...#.|...#.|...#.|...#.|...#.|.###.',
    '^': '..#..|.#.#.|#...#|.....|.....|.....|.....',
    '_': '.....|.....|.....|.....|.....|.....|#####',
    '`': '.#...|..#..|...#.|.....|.....|.....|.....',
    'a': '.....|.....|.###.|....#|.####|#...#|.####',
    'b': '#....|#....|#.##.|##..#|#...#|#...#|####.',
    'c': '.....|.....|.###.|#....|#....|#...#|.###.',
    'd': '....#|....#|.##.#|#..##|#...#|#...#|.####',
    'e': '.....|.....|.###.|#...#|#####|#....|.###.',
    'f': '..##.|.#..#|.#...|###..|.#...|.#...|.#...',
    'g': '.....|.....|.####|#...#|#...#|.####|....#|.###.',
    'h': '#....|#....|#.##.|##..#|#...#|#...#|#...#',
    'i': '..#..|.....|.##..|..#..|..#..|..#..|.###.',
    'j': '...#.|.....|..##.|...#.|...#.|...#.|#..#.|.##..',
    'k': '#....|#....|#..#.|#.#..|##...|#.#..|#..#.',
    'l': '.##..|..#..|..#..|..#..|..#..|..#..|.###.',
    'm': '.....|.....|##.#.|#.#.#|#.#.#|#.#.#|#...#',
    'n': '.....|.....|#.##.|##..#|#...#|#...#|#...#',
    'o': '.....|.....|.###.|#...#|#...#|#...#|.###.',
    'p': '.....|.....|####.|#...#|#...#|####.|#....|#....',
    'q': '.....|.....|.####|#...#|#...#|.####|....#|....#',
    'r': '.....|.....|#.##.|##..#|#....|#....|#....',
    's': '.....|.....|.####|#....|.###.|....#|####.',
    't': '.#...|.#...|###..|.#...|.#...|.#..#|..##.',
    'u': '.....|.....|#...#|#...#|#...#|#..##|.##.#',
    'v': '.....|.....|#...#|#...#|#...#|.#.#.|..#..',
    'w': '.....|.....|#...#|#...#|#.#.#|#.#.#|.#.#.',
    'x': '.....|.....|#...#|.#.#.|..#..|.#.#.|#...#',
    'y': '.....|.....|#...#|#...#|#...#|.####|....#|.###.',
    'z': '.....|.....|#####|...#.|..#..|.#...|#####',
    '{': '...#.|..#..|..#..|.#...|..#..|..#..|...#.',
    '|': '..#..|..#..|..#..|..#..|..#..|..#..|..#..',
    '}': '.#...|..#..|..#..|...#.|..#..|..#..|.#...',
    '~': '.....|.....|.#...|#.#.#|...#.|.....|.....',
    '·': '.....|.....|.....|..#..|.....|.....|.....',   // middle dot: unprintable byte
    '…': '.....|.....|.....|.....|.....|.....|#.#.#',   // ellipsis
    '␣': '.....|.....|.....|.....|.....|#...#|#####',   // visible space (in folder names)
    '▮': '#####|#####|#####|#####|#####|#####|#####',   // cursor block
  };
  // 3x5 micro font: units and tiny labels only (about 0.5 mm tall on the real panel).
  const FONT3_SRC = {
    ' ': '...|...|...|...|...',
    'A': '.#.|#.#|###|#.#|#.#', 'B': '##.|#.#|##.|#.#|##.', 'C': '.##|#..|#..|#..|.##',
    'D': '##.|#.#|#.#|#.#|##.', 'E': '###|#..|##.|#..|###', 'F': '###|#..|##.|#..|#..',
    'G': '.##|#..|#.#|#.#|.##', 'H': '#.#|#.#|###|#.#|#.#', 'I': '###|.#.|.#.|.#.|###',
    'J': '..#|..#|..#|#.#|.#.', 'K': '#.#|#.#|##.|#.#|#.#', 'L': '#..|#..|#..|#..|###',
    'M': '#.#|###|###|#.#|#.#', 'N': '##.|#.#|#.#|#.#|#.#', 'O': '.#.|#.#|#.#|#.#|.#.',
    'P': '##.|#.#|##.|#..|#..', 'Q': '.#.|#.#|#.#|##.|.##', 'R': '##.|#.#|##.|#.#|#.#',
    'S': '.##|#..|.#.|..#|##.', 'T': '###|.#.|.#.|.#.|.#.', 'U': '#.#|#.#|#.#|#.#|###',
    'V': '#.#|#.#|#.#|#.#|.#.', 'W': '#.#|#.#|###|###|#.#', 'X': '#.#|#.#|.#.|#.#|#.#',
    'Y': '#.#|#.#|.#.|.#.|.#.', 'Z': '###|..#|.#.|#..|###',
    '0': '###|#.#|#.#|#.#|###', '1': '.#.|##.|.#.|.#.|###', '2': '##.|..#|.#.|#..|###',
    '3': '##.|..#|.#.|..#|##.', '4': '#.#|#.#|###|..#|..#', '5': '###|#..|##.|..#|##.',
    '6': '.##|#..|###|#.#|###', '7': '###|..#|.#.|.#.|.#.', '8': '###|#.#|###|#.#|###',
    '9': '###|#.#|###|..#|##.',
    '/': '..#|..#|.#.|#..|#..', '.': '...|...|...|...|.#.', ':': '...|.#.|...|.#.|...',
    '-': '...|...|###|...|...', '+': '...|.#.|###|.#.|...', '(': '.#.|#..|#..|#..|.#.',
    ')': '.#.|..#|..#|..#|.#.', '%': '#.#|..#|.#.|#..|#.#', '>': '#..|.#.|..#|.#.|#..',
    '<': '..#|.#.|#..|.#.|..#', '=': '...|###|...|###|...', '_': '...|...|...|...|###',
    '!': '.#.|.#.|.#.|...|.#.', '?': '##.|..#|.#.|...|.#.', '#': '#.#|###|#.#|###|#.#',
    '·': '...|...|.#.|...|...', '×': '...|#.#|.#.|#.#|...',
  };
  function parseFont(src, w) {
    const out = {};
    for (const ch in src) {
      out[ch] = src[ch].split('|').map(row => {
        let m = 0;
        for (let i = 0; i < w; i++) if (row[i] === '#') m |= 1 << (w - 1 - i);
        return m;
      });
    }
    return out;
  }
  const FONT5 = parseFont(FONT5_SRC, 5);
  const FONT3 = parseFont(FONT3_SRC, 3);

  /* ============================================================ primitives */
  function pset(fb, x, y, c) {
    x |= 0; y |= 0;
    if (x >= 0 && y >= 0 && x < SW && y < SH) fb[y * SW + x] = c;
  }
  // Coverage-dithered pixel: a is 0..1 opacity, resolved by the Bayer matrix.
  function dset(fb, x, y, c, a) {
    x |= 0; y |= 0;
    if (x >= 0 && y >= 0 && x < SW && y < SH && (a >= 1 || bayer(x, y) < a)) fb[y * SW + x] = c;
  }
  function fillRect(fb, x, y, w, h, c, a) {
    const x0 = Math.max(0, Math.round(x)), y0 = Math.max(0, Math.round(y));
    const x1 = Math.min(SW, Math.round(x + w)), y1 = Math.min(SH, Math.round(y + h));
    if (x1 <= x0 || y1 <= y0) return;
    if (a === undefined || a >= 1) { for (let j = y0; j < y1; j++) fb.fill(c, j * SW + x0, j * SW + x1); return; }
    for (let j = y0; j < y1; j++) for (let i = x0; i < x1; i++) if (bayer(i, j) < a) fb[j * SW + i] = c;
  }
  function darkenRect(fb, x, y, w, h, amount) {
    const x0 = Math.max(0, Math.round(x)), y0 = Math.max(0, Math.round(y));
    const x1 = Math.min(SW, Math.round(x + w)), y1 = Math.min(SH, Math.round(y + h));
    const whole = Math.floor(amount), frac = amount - whole;
    for (let j = y0; j < y1; j++) for (let i = x0; i < x1; i++) {
      let n = whole + (bayer(i, j) < frac ? 1 : 0), c = fb[j * SW + i];
      while (n-- > 0) c = DARKER[c];
      fb[j * SW + i] = c;
    }
  }
  function line(fb, x0, y0, x1, y1, c, a) {
    x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
    const dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    let err = dx + dy;
    for (let guard = 0; guard < 2000; guard++) {
      dset(fb, x0, y0, c, a === undefined ? 1 : a);
      if (x0 === x1 && y0 === y1) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  function disc(fb, cx, cy, r, c, a) {
    const r2 = r * r;
    for (let y = Math.floor(cy - r); y <= Math.ceil(cy + r); y++)
      for (let x = Math.floor(cx - r); x <= Math.ceil(cx + r); x++) {
        const dx = x + 0.5 - cx, dy = y + 0.5 - cy;
        if (dx * dx + dy * dy <= r2) dset(fb, x, y, c, a === undefined ? 1 : a);
      }
  }
  // Soft radial glow drawn as dithered coverage falling off from the centre.
  function glow(fb, cx, cy, r, c, a) {
    if (a <= 0.01 || r < 0.5) return;
    for (let y = Math.floor(cy - r); y <= Math.ceil(cy + r); y++)
      for (let x = Math.floor(cx - r); x <= Math.ceil(cx + r); x++) {
        const dx = x + 0.5 - cx, dy = y + 0.5 - cy, d = Math.sqrt(dx * dx + dy * dy) / r;
        if (d < 1) dset(fb, x, y, c, a * Math.pow(1 - d, 1.6));
      }
  }
  // Flat-top hexagon outline (vertices left and right), the shape of every gallery.
  function hexOutline(fb, cx, cy, R, roll, c, a) {
    let px = 0, py = 0;
    for (let j = 0; j <= 6; j++) {
      const ang = j * Math.PI / 3 + roll;
      const x = cx + R * Math.cos(ang), y = cy + R * Math.sin(ang);
      if (j) line(fb, px, py, x, y, c, a);
      px = x; py = y;
    }
  }

  /* ================================================================== text */
  function glyph5(ch) { return FONT5[ch] || FONT5['?']; }
  function rawText5(fb, str, x, y, c, a) {
    x = Math.round(x); y = Math.round(y);
    for (const ch of str) {
      const g = glyph5(ch);
      for (let r = 0; r < g.length; r++) {
        const m = g[r];
        if (m) for (let i = 0; i < 5; i++) if (m & (16 >> i)) dset(fb, x + i, y + r, c, a === undefined ? 1 : a);
      }
      x += 6;
    }
    return x;
  }
  // Text with a 1px dark keyline so it reads over the shaft.
  function text5(fb, str, x, y, c, outline, a) {
    if (outline !== undefined && outline >= 0) {
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++)
        if (dx || dy) rawText5(fb, str, x + dx, y + dy, outline, a);
    }
    return rawText5(fb, str, x, y, c, a);
  }
  // Runs of [char, colour] pairs, for the path bar and content line.
  function runs5(fb, runs, x, y, outline) {
    x = Math.round(x);
    if (outline !== undefined) {
      let ox = x;
      for (const [ch] of runs) {
        for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) if (dx || dy) rawText5(fb, ch, ox + dx, y + dy, outline);
        ox += 6;
      }
    }
    for (const [ch, c] of runs) { rawText5(fb, ch, x, y, c); x += 6; }
    return x;
  }
  function text3(fb, str, x, y, c, a) {
    x = Math.round(x); y = Math.round(y);
    for (const ch0 of str) {
      const g = FONT3[ch0.toUpperCase()] || FONT3['?'];
      for (let r = 0; r < 5; r++) for (let i = 0; i < 3; i++)
        if (g[r] & (4 >> i)) dset(fb, x + i, y + r, c, a === undefined ? 1 : a);
      x += 4;
    }
    return x;
  }
  const w5 = s => [...s].length * 6 - 1;
  const w3 = s => [...s].length * 4 - 1;

  /* ============================================================ encodings */
  // HEX:  256 folders per level named 00..FF, one byte per level (the version
  //       described in the brief).
  // B70:  p2r3/babel-usb as shipped: 70-symbol alphabet, two-character names,
  //       70^2 = 4900 folders + 1 `file` per directory, path read as a bijective
  //       base-4900 number and written out as bijective base-256 bytes
  //       (fs_append_output() in usb_mtp_impl.c.h, file-to-path.js).
  const ALPHA70 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789! &()-_+';
  const HEX2 = Array.from({ length: 256 }, (_, i) => i.toString(16).toUpperCase().padStart(2, '0'));
  const ENCODINGS = {
    hex: {
      id: 'hex', label: 'HEX', fanout: 256, objects: 257,
      name: c => HEX2[c],
      content: path => path.slice(),
      pathFor: bytes => Array.from(bytes),
      random: r => (r() * 256) | 0,
    },
    b70: {
      id: 'b70', label: 'BABEL-70', fanout: 4900, objects: 4901,
      name: h => { const i = h - 1; return ALPHA70[(i / 70) | 0] + ALPHA70[i % 70]; },
      content: path => {
        let n = 0n;
        for (const h of path) n = n * 4900n + BigInt(h);
        const out = [];
        while (n > 0n) { n -= 1n; out.push(Number(n % 256n)); n /= 256n; }
        return out;
      },
      pathFor: bytes => {
        let n = 0n;
        for (let i = bytes.length - 1; i >= 0; i--) n = n * 256n + BigInt(bytes[i] + 1);
        const p = [];
        while (n > 0n) { n -= 1n; p.unshift(Number(n % 4900n) + 1); n /= 4900n; }
        return p;
      },
      random: r => 1 + ((r() * 4900) | 0),
    },
  };
  const UTF8 = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;
  const textBytes = s => (UTF8 ? Array.from(UTF8.encode(s)) : Array.from(s, ch => ch.charCodeAt(0) & 255));

  /* ============================================================ the shaft */
  // Texture space: u runs around the hexagon (6 walls x TW texels), v runs down
  // the shaft (F texels per level). One level = railing, floor slab, then five
  // shelves of books ("twenty shelves, five long shelves per side").
  const F = 48, TW = 48, LW = TW * 6;
  const RAIL_FV = 1 / F, LAMP_FV = 3 / F, TAB_FV = 1 / F;
  const K_BASE = 140;             // ring radius in px at a depth of one level (a long lens:
                                  // keeps texels near 2px at the screen edge)
  const VP_X = 120, VP_Y = 52;    // vanishing point sits high to clear the catalog card
  const HUD_Y = 111;
  const FALL_SPEED = 0.55;        // idle freefall, levels per second
  const Z_FOG0 = 2.8, Z_FOG1 = 12;
  const Z_NEXT = 3.3;             // at rest, the next floor's railing sits this deep
  const Z_LAND = 2.75;            // a folder tab hands off to the path bar here
  const Z_LOCK = 3.7;             // idle wander picks its next door here
  const Z_TAB_MAX = 3.72;         // tabs further down stay hidden
  const fog = z => (z <= Z_FOG0 ? 1 : z >= Z_FOG1 ? 0 : Math.pow((Z_FOG1 - z) / (Z_FOG1 - Z_FOG0), 2));
  // The falling reader's own lantern: lights the near walls, never the depths.
  const lantern = z => { const q = 1 - (z - 1) / 3.4; return q <= 0 ? 0 : 0.8 * Math.pow(Math.min(q, 1), 1.4); };

  // Colour ramps, darkest to brightest. Shadows drift toward the cool neutrals,
  // lamplight pushes toward amber.
  const R_DARK = 0, R_RAIL = 1, R_WOOD = 2, R_RED = 3, R_GREEN = 4, R_BLUE = 5, R_GOLD = 6, R_PAPER = 7;
  // Every ramp passes through the cool neutrals before reaching its own hue,
  // so whatever the lamps don't reach falls back to blue-black.
  const RAMPS = [
    [VOID, INK, SHADE],
    [VOID, INK, SHADE, UMBER, WOOD, EMBER, LAMP],
    [VOID, INK, SHADE, UMBER, WOOD, EMBER],
    [VOID, INK, SHADE, OXBLOOD, EMBER],
    [VOID, INK, SHADE, MOSS, OCHRE],
    [VOID, INK, SHADE, DUSK, INDIGO],
    [VOID, INK, UMBER, WOOD, OCHRE, LAMP],
    [VOID, INK, SHADE, DUSK, VELLUM],
  ];
  const NOM = [2, 4, 4, 3, 3, 4, 4, 3];                 // each ramp's true colour
  const nb = r => NOM[r] / (RAMPS[r].length - 1);       // brightness of that colour at light 1
  const BOOK_RAMPS = [R_RED, R_RED, R_RED, R_RED, R_GREEN, R_GREEN, R_BLUE, R_BLUE, R_BLUE, R_GOLD, R_WOOD, R_WOOD, R_PAPER];

  function genWall(seed, door) {
    const rnd = rng(seed);
    const ramp = new Uint8Array(F * TW), bri = new Float32Array(F * TW);
    const put = (r, c, rp, b) => { ramp[r * TW + c] = rp; bri[r * TW + c] = b; };
    for (let c = 0; c < TW; c++) {
      put(0, c, R_RAIL, nb(R_RAIL) * 1.15);                   // top rail, catches the lamps
      put(1, c, R_RAIL, nb(R_RAIL) * 0.8);
      const post = (c & 3) === 1;                             // balusters; the gallery floor shows between
      for (let r = 2; r <= 4; r++) put(r, c, R_WOOD, post ? nb(R_WOOD) * (r === 2 ? 1 : 0.85) : nb(R_WOOD) * 0.42);
      put(5, c, R_WOOD, nb(R_WOOD) * 0.95);                  // slab edge
      put(6, c, R_WOOD, nb(R_WOOD) * 0.68);
      put(7, c, R_DARK, 0.45);
    }
    for (let s = 0; s < 5; s++) {
      const top = 8 + s * 8;
      for (let c = 0; c < TW; c++) {
        put(top, c, R_DARK, 0.22);
        for (let r = top + 1; r < top + 7; r++) put(r, c, R_DARK, 0.5);
        put(top + 7, c, R_WOOD, nb(R_WOOD) * 0.9);            // the shelf board
      }
      let c = 0;
      while (c < TW) {
        if (rnd() < 0.07) { c += 1 + ((rnd() * 2) | 0); continue; }
        const w = [2, 2, 3, 3, 3, 4][(rnd() * 6) | 0];
        const h = 4 + ((rnd() * 3) | 0);
        const rp = BOOK_RAMPS[(rnd() * BOOK_RAMPS.length) | 0];
        const tone = 0.86 + rnd() * 0.28;
        const gilt = rnd() < 0.6;
        const gRow = top + 7 - h + 1 + ((rnd() * 2) | 0);
        for (let i = 0; i < w && c + i < TW; i++) {
          for (let r = top + 7 - h; r < top + 7; r++) {
            if (gilt && r === gRow && i > 0) put(r, c + i, R_GOLD, nb(R_GOLD) * 0.95);
            else put(r, c + i, rp, nb(rp) * tone * (i === 0 ? 0.68 : 1));
          }
        }
        c += w;
      }
    }
    if (door) {
      // One of the two free sides: the hallway to the next gallery.
      for (let r = 8; r < F; r++) for (let c = 9; c <= 22; c++) put(r, c, R_DARK, 0.06);
      for (let r = 8; r < F; r++) { put(r, 8, R_WOOD, nb(R_WOOD) * 0.7); put(r, 23, R_WOOD, nb(R_WOOD) * 0.7); }
      for (let c = 8; c <= 23; c++) { put(8, c, R_WOOD, nb(R_WOOD) * 0.85); put(9, c, R_WOOD, nb(R_WOOD) * 0.55); }
    }
    return { rows: F, cols: TW, ramp, bri };
  }
  // Box-filtered mips: the dominant material wins, brightness is averaged.
  // Without these the far shelves shimmer as the camera moves.
  function buildMips(base) {
    const mips = [base];
    for (let m = 1; m <= 4; m++) {
      const s = 1 << m, rows = F >> m, cols = TW >> m;
      const ramp = new Uint8Array(rows * cols), bri = new Float32Array(rows * cols);
      const weight = new Float32Array(RAMPS.length);
      for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) {
        weight.fill(0);
        let sum = 0;
        for (let i = 0; i < s; i++) for (let j = 0; j < s; j++) {
          const idx = (r * s + i) * TW + (c * s + j);
          weight[base.ramp[idx]] += base.bri[idx] + 0.02;
          sum += base.bri[idx];
        }
        let best = 0;
        for (let k = 1; k < RAMPS.length; k++) if (weight[k] > weight[best]) best = k;
        ramp[r * cols + c] = best;
        bri[r * cols + c] = sum / (s * s);
      }
      mips.push({ rows, cols, ramp, bri });
    }
    return mips;
  }
  const NVAR = 16, NDOOR = 4;
  const WALLS = Array.from({ length: NVAR }, (_, i) => buildMips(genWall(0x51ab + i * 7919, false)));
  const DOORS = Array.from({ length: NDOOR }, (_, i) => buildMips(genWall(0x7a11 + i * 104729, true)));
  const DOOR_WALL_A = 1, DOOR_WALL_B = 4;   // bottom and top walls are the free sides
  function wallTex(L, k) {
    if (k === DOOR_WALL_A || k === DOOR_WALL_B) return DOORS[hash(L, k) % NDOOR];
    return WALLS[hash(L, k) % NVAR];
  }

  // Light maps: "two spherical lamps per hexagon, placed transversally". The
  // lamps sit on the left and right corners of each railing. Contributions from
  // the same level, the level above and the level below are kept apart so each
  // floor's lamps can ignite on their own during the connect cascade.
  const LM0 = new Float32Array(F * LW), LMU = new Float32Array(F * LW), LMD = new Float32Array(F * LW);
  (function buildLight() {
    const sig2 = 2 * 0.34 * 0.34, LEVEL_H = 1.25;
    for (let r = 0; r < F; r++) {
      const fv = (r + 0.5) / F;
      for (let col = 0; col < LW; col++) {
        const u = (col + 0.5) / TW;
        for (const lu of [0, 3]) {
          let du = Math.abs(u - lu); if (du > 3) du = 6 - du;
          for (let dl = -1; dl <= 1; dl++) {
            const dv = (fv - (LAMP_FV + dl)) * LEVEL_H;
            const d2 = du * du + dv * dv;
            const I = 0.62 * Math.exp(-d2 / sig2) + 0.12 / (1 + 5 * d2);
            (dl === 0 ? LM0 : dl < 0 ? LMU : LMD)[r * LW + col] += I;
          }
        }
      }
    }
  })();

  // Lens vignette, fixed to the glass: pulls the eye toward the book.
  const VIG = new Float32Array(SW * SH);
  for (let y = 0; y < SH; y++) for (let x = 0; x < SW; x++) {
    const dx = (x + 0.5 - 120) / 120, dy = (y + 0.5 - 58) / 70;
    VIG[y * SW + x] = 1 - 0.34 * Math.pow(Math.min(1.3, dx * dx + dy * dy), 1.25);
  }

  // Tangent directions of the six walls (clockwise from the right-hand corner).
  const TANG = [[-0.5, 0.8660254], [-1, 0], [-0.5, -0.8660254], [0.5, -0.8660254], [1, 0], [0.5, 0.8660254]];

  function renderShaft(fb, P) {
    const KA = P.K * 0.8660254, cr = Math.cos(P.roll), sr = Math.sin(P.roll);
    let cacheL = 1e9, i0 = 0, iu = 0, id = 0;
    for (let y = 0; y < SH; y++) {
      const dy0 = y + 0.5 - P.vpy;
      for (let x = 0; x < SW; x++) {
        const dx0 = x + 0.5 - P.vpx;
        const rx = dx0 * cr + dy0 * sr, ry = -dx0 * sr + dy0 * cr;
        const p1 = 0.8660254 * rx + 0.5 * ry, p2 = -0.8660254 * rx + 0.5 * ry;
        let a = p1, k = 0;
        if (ry > a) { a = ry; k = 1; }
        if (p2 > a) { a = p2; k = 2; }
        if (-p1 > a) { a = -p1; k = 3; }
        if (-ry > a) { a = -ry; k = 4; }
        if (-p2 > a) { a = -p2; k = 5; }
        if (a < 0.35) a = 0.35;
        const z = KA / a;                       // depth below the camera, in levels
        const fz = fog(z);
        const idx = y * SW + x;
        if (fz <= 0) { fb[idx] = VOID; continue; }
        const e = TANG[k];
        let t = ((rx * e[0] + ry * e[1]) / (a * 0.5773503) + 1) * 0.5;
        t = t < 0 ? 0 : t > 0.99999 ? 0.99999 : t;
        const v = P.camZ + z, L = Math.floor(v), fv = v - L;
        const footV = F * KA / (a * a), footU = TW * 0.8660254 / a;
        const foot = (footV > footU ? footV : footU) * 0.8;
        const mip = wallTex(L, k)[foot < 2 ? 0 : foot < 4 ? 1 : foot < 8 ? 2 : foot < 16 ? 3 : 4];
        const ti = ((fv * mip.rows) | 0) * mip.cols + ((t * mip.cols) | 0);
        if (L !== cacheL) { cacheL = L; i0 = P.lampI(L); iu = P.lampI(L - 1); id = P.lampI(L + 1); }
        const li = ((fv * F) | 0) * LW + (((k + t) * TW) | 0);
        const light = P.ambient + i0 * LM0[li] + iu * LMU[li] + id * LMD[li] + P.lantern * lantern(z);
        const ramp = RAMPS[mip.ramp[ti]], n = ramp.length - 1;
        let pos = mip.bri[ti] * light * fz * VIG[idx] * n;
        if (pos > n) pos = n;
        const ip = pos | 0;
        fb[idx] = ramp[(pos - ip) > bayer(x, y) && ip < n ? ip + 1 : ip];
      }
    }
  }

  // Screen position of a point on the hexagon (u in wall units 0..6) at depth z.
  function shaftPoint(P, u, z, out) {
    const R = P.K / z;
    const k = Math.floor(u) % 6, t = u - Math.floor(u);
    const a0 = k * Math.PI / 3, a1 = (k + 1) * Math.PI / 3;
    const x = R * lerp(Math.cos(a0), Math.cos(a1), t), y = R * lerp(Math.sin(a0), Math.sin(a1), t);
    const cr = Math.cos(P.roll), sr = Math.sin(P.roll);
    out[0] = P.vpx + x * cr - y * sr;
    out[1] = P.vpy + x * sr + y * cr;
    return out;
  }

  /* ======================================================= shaft overlays */
  // Far railings drawn as crisp hexagons so the infinite repeat stays legible
  // after the texture has dissolved into fog.
  function drawFarRings(fb, P) {
    for (let L = Math.floor(P.camZ) + 1; L < P.camZ + 26; L++) {
      const z = L + RAIL_FV - P.camZ;
      if (z < 10) continue;
      const lit = P.lampI(L);
      const f = clamp(1 - (z - 10) / 16, 0, 1) * clamp((z - 10) / 2, 0, 1);
      hexOutline(fb, P.vpx, P.vpy, P.K / z, P.roll, lit > 0.5 ? UMBER : SHADE, f * 0.55);
    }
  }

  const _pt = [0, 0];
  function drawLamps(fb, P) {
    for (let L = Math.floor(P.camZ); L < P.camZ + 12; L++) {
      const I = P.lampI(L);
      if (I < 0.03) continue;
      const z = L + LAMP_FV - P.camZ;
      if (z < 0.9) continue;
      const f = fog(z) * I;
      const r = clamp(5.2 / z, 0.45, 4.2);
      for (const u of [0, 3]) {
        shaftPoint(P, u, z, _pt);
        const x = _pt[0], y = _pt[1];
        if (x < -20 || x > SW + 20 || y < -20 || y > SH + 20) continue;
        glow(fb, x, y, r * 3.2, EMBER, 0.42 * f);
        glow(fb, x, y, r * 1.8, LAMP, 0.6 * f);
        if (r >= 0.9) disc(fb, x, y, r * 0.8, LAMP, Math.min(1, f * 1.4));
        else dset(fb, x, y, LAMP, f * 1.5);
        if (r > 1.7) disc(fb, x, y, r * 0.38, WHITE, Math.min(1, f * 1.6));
      }
    }
  }

  function drawParticles(fb, P, parts, t) {
    for (const p of parts) {
      const z = p.v - P.camZ;
      if (z < 1.2 || z > 15) continue;
      const R = (P.K / z) * p.rho;
      const ang = p.phi + P.roll;
      const x = P.vpx + R * Math.cos(ang), y = P.vpy + R * Math.sin(ang);
      const near = clamp((z - 1.2) / 0.8, 0, 1);
      const a = fog(z) * near * p.a;
      if (a < 0.05) continue;
      const ch = '0123456789ABCDEF'[(p.g + Math.floor(t * p.flip)) & 15];
      text3(fb, ch, x - 1, y - 2, z < 4.5 ? CYAN : DUSK, Math.min(1, a * 1.3));
    }
  }

  // The book at the bottom of the shaft: a closed volume lying face up,
  // oxblood cloth, gilt frame, a hexagon stamped on the cover.
  const SMALL_BOOK = [
    '.kRRRRRRRR.',
    'kkRgggggggR',
    'kkRgRRhRRgR',
    'kkRgRhRhRgR',
    'kkRgRRhRRgR',
    'kkRgggggggR',
    'kkRRRRRRRRv',
    '.vvvvvvvvvv',
  ];
  const BOOK_MAP = { k: UMBER, R: OXBLOOD, g: OCHRE, h: LAMP, v: VELLUM };
  function drawSmallBook(fb, x, y, warm, t) {
    const pulse = 0.5 + 0.5 * Math.sin(t * 2.1);
    glow(fb, x, y, 15, warm ? UMBER : SHADE, 0.6);
    glow(fb, x, y, 11, warm ? EMBER : DUSK, 0.32 + 0.18 * pulse);
    glow(fb, x, y, 7, LAMP, (warm ? 0.3 : 0.16) + 0.14 * pulse);
    const ox = Math.round(x - 5), oy = Math.round(y - 4);
    for (let r = 0; r < SMALL_BOOK.length; r++)
      for (let c = 0; c < SMALL_BOOK[r].length; c++) {
        const ch = SMALL_BOOK[r][c];
        if (ch !== '.') pset(fb, ox + c, oy + r, BOOK_MAP[ch]);
      }
  }

  // Folder-name tab: a 2-character plate riding the railing, with a folder notch.
  function drawTab(fb, cx, cy, text, style, a) {
    const x0 = Math.round(cx - 7), y0 = Math.round(cy - 5);
    const fill = style === 'flash' ? WHITE : INK;
    const edge = style === 'known' ? CYAN : style === 'flash' ? WHITE : DUSK;
    for (let i = 1; i <= 5; i++) dset(fb, x0 + i, y0 - 1, edge, a);            // folder notch
    for (let j = 0; j < 11; j++) for (let i = 0; i < 15; i++) {
      const border = i === 0 || j === 0 || i === 14 || j === 10;
      dset(fb, x0 + i, y0 + j, border ? edge : fill, a);
    }
    let tx = x0 + 2;
    for (const ch of text) {
      const shown = ch === ' ' ? '␣' : ch;
      const col = style === 'flash' ? INK : ch === ' ' ? DUSK : style === 'known' ? CYAN : (style === 'flicker2' ? DUSK : VELLUM);
      rawText5(fb, shown, tx, y0 + 2, col, a);
      tx += 6;
    }
  }

  /* ============================================================= the book */
  const PAGE_W = 70, PAGE_H = 84;
  function drawOpenBook(fb, sc, P) {
    const b = sc.book;
    const t = sc.t;
    const RISE = 0.5, OPEN = 0.36, SHUT = 0.3, SINK = 0.42;
    let rise, open;
    const ta = t - b.openAt;
    if (b.closeAt < 0 || t < b.closeAt) { rise = clamp(ta / RISE, 0, 1); open = clamp((ta - RISE) / OPEN, 0, 1); }
    else { const tc = t - b.closeAt; open = 1 - clamp(tc / SHUT, 0, 1); rise = 1 - clamp((tc - SHUT) / SINK, 0, 1); }
    if (rise <= 0) return false;
    const er = easeOut(rise);
    darkenRect(fb, 0, 0, SW, HUD_Y, 2.2 * er);

    // Closed volume grows from the speck at the vanishing point.
    const cx = 120, cy = 56;
    const sx = lerp(P.vpx - 5, cx, er), sy = lerp(P.vpy - 4, cy - PAGE_H / 2, er);
    const w = lerp(11, PAGE_W, er), h = lerp(8, PAGE_H, er);
    const theta = easeInOut(open) * Math.PI;
    const ct = Math.cos(theta), st = Math.sin(theta);

    // Right-hand page (under the cover).
    if (open > 0) page(fb, sx, sy, w, h, 'right', sc, open);
    // Cover, or the left page once the cover passes vertical.
    const lift = 7 * st;
    if (ct >= 0) {
      quad(fb, sx, sy, h, sx + w * ct, h + lift, (fx, fy, gx, gy) => coverPixel(fx, fy, gx, gy, w * ct, h, er));
      // page block edge visible beside the cover
      if (open === 0) {
        fillRect(fb, sx + w, sy + 2, Math.max(1, er * 3), h - 2, VELLUM);
        fillRect(fb, sx + 2, sy + h, w - 1, Math.max(1, er * 2), DUSK);
      }
    } else {
      page(fb, sx + w * ct, sy - lift / 2, -w * ct, h + lift, 'left', sc, open, true);
    }
    // the cover is stamped with the name of the folder the book lives in
    if (ct > 0.97 && er > 0.85) {
      const title = sc.reveal ? sc.enc.name(sc.path[sc.reveal - 1]) : '/';
      const tw = w5(title), ex = sx + w * 0.53, ey = sy + h * 0.4 + 13;
      let tx = Math.round(ex - tw / 2);
      for (const ch of title) { rawText5(fb, ch === ' ' ? '␣' : ch, tx, Math.round(ey), OCHRE, clamp((er - 0.85) / 0.15, 0, 1)); tx += 6; }
    }
    // the spine
    if (er > 0.3) fillRect(fb, sx - 1, sy - 1, 2, h + 2, UMBER);
    return true;
  }
  // Fill a trapezoid that hinges on the spine: left edge (x0, height h0), free edge (x1, h1).
  function quad(fb, x0, y0, h0, x1, h1, shade) {
    const xa = Math.min(x0, x1), xb = Math.max(x0, x1);
    const cy = y0 + h0 / 2;
    for (let x = Math.floor(xa); x < Math.ceil(xb); x++) {
      const f = xb - xa < 1 ? 0 : (x + 0.5 - x0) / (x1 - x0);
      const hh = lerp(h0, h1, clamp(f, 0, 1));
      const top = cy - hh / 2;
      for (let y = Math.floor(top); y < Math.ceil(top + hh); y++) {
        const c = shade(clamp(f, 0, 1), (y + 0.5 - top) / hh, x, y);
        if (c >= 0) pset(fb, x, y, c);
      }
    }
  }
  function coverPixel(fx, fy, x, y, w, h, er) {
    // gilt frame inset 4px, hexagon emblem, a title in the gilt
    const px = fx * Math.abs(w), py = fy * h;
    const W = Math.abs(w);
    if (px < 2) return UMBER;                                    // spine shadow
    if (W > 20 && ((Math.abs(px - 5) < 0.5 || Math.abs(px - (W - 4)) < 0.5) && py > 4 && py < h - 4)) return OCHRE;
    if (W > 20 && ((Math.abs(py - 4.5) < 0.5 || Math.abs(py - (h - 4.5)) < 0.5) && px > 4.5 && px < W - 3.5)) return OCHRE;
    // emblem: hexagon ring centred at 42% height
    if (W > 30) {
      const ex = px - W * 0.53, ey = py - h * 0.40;
      const ap = Math.max(Math.abs(ey), 0.866 * Math.abs(ex) + 0.5 * Math.abs(ey));
      if (ap > 7.2 && ap < 8.6) return OCHRE;
      if (ap < 3) return LAMP;
    }
    return bayer(x, y) < 0.18 ? UMBER : OXBLOOD;
  }
  function page(fb, x, y, w, h, side, sc, open, flipped) {
    if (w < 1) return;
    fillRect(fb, x, y, w, h, VELLUM);
    // gutter shadow toward the spine
    for (let i = 0; i < 4; i++) {
      const gx = side === 'right' ? x + i : x + w - 1 - i;
      fillRect(fb, gx, y, 1, h, DUSK, 0.45 * (1 - i / 4));
    }
    // deckle: bottom and outer edges
    fillRect(fb, x, y + h - 1, w, 1, OCHRE, 0.5);
    if (open < 1 || w < PAGE_W - 2) return;
    const fade = clamp((sc.t - sc.book.openAt - 0.86) / 0.12, 0, 1);
    if (fade <= 0) return;
    const bytes = sc.content();
    if (side === 'left') {
      // verso: the raw bytes, laid out like a hex editor
      const tx = x + 6;
      text5(fb, 'file', tx, y + 6, OXBLOOD, undefined, fade);
      const size = bytes.length + ' B';
      text3(fb, size.toUpperCase(), x + w - 6 - w3(size), y + 8, DUSK, fade);
      for (let i = x + 6; i < x + w - 6; i++) dset(fb, i, y + 16, DUSK, 0.6 * fade);
      const per = 3, maxLines = 7;
      const lines = Math.min(maxLines, Math.ceil(bytes.length / per));
      for (let l = 0; l < lines; l++) {
        const chunk = bytes.slice(l * per, l * per + per);
        let s = chunk.map(b => HEX2[b]).join(' ');
        if (l === maxLines - 1 && bytes.length > per * maxLines) s = s.slice(0, 5) + ' …';
        text3(fb, HEX2[(l * per) & 255], tx, y + 22 + l * 9, DUSK, fade);
        text5(fb, s, tx + 10, y + 21 + l * 9, INK, undefined, fade);
      }
      if (!bytes.length) text5(fb, '(empty)', tx, y + 21, DUSK, undefined, fade);
    } else {
      // recto: the same bytes as text, opened with an illuminated initial
      const chars = bytes.map(b => (b >= 32 && b < 127 ? String.fromCharCode(b) : '·'));
      const tx = x + 9;
      let rest = chars, lineX = [tx, tx], lineN = [9, 9];
      if (chars.length && chars[0] !== ' ' && chars[0] !== '·') {
        bigGlyph(fb, chars[0], tx + 1, y + 9, OCHRE, 2, fade);
        bigGlyph(fb, chars[0], tx, y + 8, OXBLOOD, 2, fade);
        rest = chars.slice(1); lineX = [tx + 13, tx + 13]; lineN = [7, 7];
      }
      let i = 0;
      for (let l = 0; l < 7 && i < rest.length; l++) {
        const n = l < 2 ? lineN[l] : 9;
        let xx = l < 2 ? lineX[l] : tx;
        const seg = rest.slice(i, i + n);
        i += n;
        if (l === 6 && i < rest.length) seg[seg.length - 1] = '…';
        for (const ch of seg) { rawText5(fb, ch, xx, y + 9 + l * 9 + (l < 2 ? 3 : 0), ch === '·' || ch === '…' ? DUSK : INK, fade); xx += 6; }
      }
      if (!chars.length) text5(fb, '(empty)', tx, y + 9, DUSK, undefined, fade);
    }
  }
  function bigGlyph(fb, ch, x, y, c, s, a) {
    const g = glyph5(ch);
    for (let r = 0; r < g.length; r++) for (let i = 0; i < 5; i++)
      if (g[r] & (16 >> i)) for (let dy = 0; dy < s; dy++) for (let dx = 0; dx < s; dx++) dset(fb, x + i * s + dx, y + r * s + dy, c, a);
  }

  /* ================================================================== HUD */
  const PATH_X = 5, PATH_Y = 114, BODY_Y = 125, PATH_MAX = 31;

  function nameRuns(name) {
    const out = [];
    for (const ch of name) out.push(ch === ' ' ? ['␣', DUSK] : [ch, CYAN]);
    return out;
  }
  // Lay out "/48/65/6C" with a leading ellipsis when it overflows; report where
  // each element's name starts so flyers know where to land.
  function layoutPath(names, maxChars) {
    const all = [], slots = [];
    for (let i = 0; i < names.length; i++) {
      all.push(['/', DUSK]);
      slots.push(all.length);
      for (const r of nameRuns(names[i])) all.push(r);
    }
    if (!names.length) all.push(['/', DUSK]);
    let cut = 0;
    if (all.length > maxChars) {
      const need = all.length - (maxChars - 1);
      // cut at an element boundary so a name is never split
      let i = 0;
      while (i < slots.length && slots[i] - 1 < need) i++;
      cut = i < slots.length ? slots[i] - 1 : need;
    }
    const runs = cut ? [['…', DUSK]].concat(all.slice(cut)) : all;
    const off = cut ? cut - 1 : 0;
    return { runs, slotX: slots.map(s => (s - off >= (cut ? 1 : 0) ? PATH_X + (s - off) * 6 : null)) };
  }

  function drawHUD(fb, sc, P) {
    fillRect(fb, 0, HUD_Y, SW, SH - HUD_Y, INK);
    fillRect(fb, 0, HUD_Y, SW, 1, SHADE);
    // catalog-card rule between call number and title
    for (let x = 3; x < 197; x++) dset(fb, x, HUD_Y + 11, OXBLOOD, 0.5);

    // call number: the path
    const names = sc.path.slice(0, sc.reveal).map(c => sc.enc.name(c));
    let wipeNames = null;
    if (sc.wipe) {
      const p = clamp((sc.t - sc.wipe.t0) / sc.wipe.dur, 0, 1);
      if (p < 1) wipeNames = sc.wipe.names.slice(0, Math.ceil(sc.wipe.names.length * (1 - easeIn(p))));
    }
    const lay = layoutPath(wipeNames || names, PATH_MAX);
    const endX = runs5(fb, lay.runs, PATH_X, PATH_Y);
    if (sc.host && !wipeNames && Math.floor(sc.t * 1.7) % 2 === 0) fillRect(fb, endX, PATH_Y + 6, 5, 1, CYAN);

    // title: the file at the bottom of this folder, as text
    const toast = sc.toast && sc.t - sc.toast.t0 < sc.toast.dur ? sc.toast : null;
    if (toast) {
      const blink = sc.t - toast.t0 < 0.5 ? Math.floor((sc.t - toast.t0) * 12) % 2 : 0;
      text5(fb, toast.text, PATH_X, BODY_Y, blink ? VELLUM : toast.color);
    } else {
      const bytes = sc.content();
      const max = PATH_MAX;
      let list = bytes;
      let x = PATH_X;
      if (bytes.length > max) { rawText5(fb, '…', x, BODY_Y, DUSK); x += 6; list = bytes.slice(bytes.length - (max - 1)); }
      if (!bytes.length) rawText5(fb, sc.host ? 'empty file' : '', x, BODY_Y, DUSK);
      const fresh = sc.t - sc.revealAt < 0.35;
      list.forEach((b, i) => {
        const printable = b >= 32 && b < 127;
        const last = i === list.length - 1;
        const col = !printable ? DUSK : last && fresh ? WHITE : VELLUM;
        rawText5(fb, printable ? String.fromCharCode(b) : '·', x, BODY_Y, col);
        x += 6;
      });
    }

    // depth and size, right-aligned
    const depth = String(sc.reveal).padStart(2, '0');
    const size = String(sc.content().length);
    text3(fb, 'LV', 205, PATH_Y + 1, DUSK);
    text5(fb, depth, 236 - w5(depth), PATH_Y, sc.host ? VELLUM : DUSK);
    text5(fb, size, 230 - w5(size), BODY_Y, sc.host ? VELLUM : DUSK);
    text3(fb, 'B', 232, BODY_Y + 2, DUSK);
  }

  function drawTopBar(fb, sc) {
    // wordmark: a gallery and the device name from DEV_INFO_MODEL
    const hx = 3, hy = 2;
    // a gallery seen from above, with the shaft in its middle
    const HEXICON = ['..#####..', '.#.....#.', '.#..o..#.', '#..ooo..#', '#..ooo..#', '.#..o..#.', '.#.....#.', '..#####..'];
    for (let r = 0; r < 8; r++) for (let c = 0; c < 9; c++) {
      if (HEXICON[r][c] !== '.') {
        for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) pset(fb, hx + c + dx, hy - 1 + r + dy, VOID);
      }
    }
    for (let r = 0; r < 8; r++) for (let c = 0; c < 9; c++) {
      const ch = HEXICON[r][c];
      if (ch === '#') pset(fb, hx + c, hy - 1 + r, OCHRE);
      else if (ch === 'o') pset(fb, hx + c, hy - 1 + r, sc.host ? LAMP : DUSK);
    }
    text5(fb, 'USB OF BABEL', 15, 2, OCHRE, VOID);

    // status: session + activity LED
    const led = sc.t - sc.ledAt < 0.09 ? CRIMSON : sc.host ? CYAN : DUSK;
    const label = sc.host ? 'MTP' : 'NO HOST';
    const lx = 236 - w5(label);
    text5(fb, label, lx, 2, sc.host ? VELLUM : DUSK, VOID);
    fillRect(fb, lx - 7, 3, 5, 5, VOID);
    fillRect(fb, lx - 6, 4, 3, 3, led);
  }

  function drawFlyers(fb, sc) {
    for (const f of sc.flyers) {
      const p = clamp((sc.t - f.t0) / f.dur, 0, 1);
      const e = easeInOut(p);
      const x = lerp(f.x0, f.x1, e), y = lerp(f.y0, f.y1, e) - Math.sin(p * Math.PI) * f.arc;
      if (f.kind === 'land') {
        if (p < 0.45) drawTab(fb, x + 7 - 2, y + 5 - 2, f.text, 'known', 1);
        else runs5(fb, nameRuns(f.text), x, y, VOID);
      } else {
        runs5(fb, nameRuns(f.text), x, y - p * 10, VOID);
      }
    }
  }

  /* ================================================================ scene */
  function Scene(opts) {
    opts = opts || {};
    this.enc = ENCODINGS[opts.mode] || ENCODINGS.hex;
    this.seed = opts.seed || 1;
    this.rnd = rng(this.seed * 7919 + 17);
    this.t = 0;
    this.host = false;
    this.path = [];
    this.reveal = 0;
    this.revealAt = -9;
    this.levelBase = 0;
    this.camZ = 0;
    this.vel = FALL_SPEED;
    this.queue = [];
    this.act = null;
    this.flyers = [];
    this.landed = new Set();
    this.lamp = { on: false, t0: -99, origin: 0, from: 0 };
    this.lanternState = { on: true, t0: -99 };
    this.ledAt = -9;
    this.book = { openAt: -99, closeAt: -99, isOpen: false };
    this.toast = null;
    this.wipe = null;
    this.lockFx = null;
    this.unlockFx = null;
    this.fovKick = 0;
    this.tilt = { x: 0, y: 0, roll: 0 };
    this.tiltS = { x: 0, y: 0, roll: 0 };
    this.autoTilt = true;
    this.tiltFn = null;     // optional t -> {x, y, roll}; the GIF export uses a periodic one
    this.bobFreq = 1.35;
    this.events = [];
    this.pathVersion = 0;   // any change to the path (content cache)
    this.epoch = 0;         // destructive changes only: in-flight flyers from older epochs never land
    this._content = { v: -1, n: -1, bytes: [] };
    this.particles = [];
    for (let i = 0; i < 14; i++) this.particles.push(this._particle(true));
    this.director = null;
  }
  const S = Scene.prototype;

  S._particle = function (initial) {
    const r = this.rnd;
    return {
      phi: r() * Math.PI * 2, rho: 0.22 + r() * 0.62,
      v: this.camZ + (initial ? 1.5 + r() * 13 : 10 + r() * 5),
      speed: 0.18 + r() * 0.22, g: (r() * 16) | 0, flip: 1 + r() * 3, a: 0.35 + r() * 0.45,
    };
  };
  S.rest = function (depth) { return this.levelBase + depth + 1 - Z_NEXT; };
  S.emit = function (op, detail) {
    this.events.push({ t: this.t, op, detail: detail || '' });
    if (this.events.length > 60) this.events.shift();
    this.ledAt = this.t;
  };
  S.content = function () {
    if (this._content.v !== this.pathVersion || this._content.n !== this.reveal) {
      this._content = { v: this.pathVersion, n: this.reveal, bytes: this.enc.content(this.path.slice(0, this.reveal)) };
    }
    return this._content.bytes;
  };
  S.handleFor = function (depth, comp) {
    // usb_mtp_impl.c.h numbers objects i + 1 + depth * handle_count
    const objects = this.enc.objects;
    const i = this.enc.id === 'b70' ? comp - 1 : comp;
    return '0x' + ((i + 1 + depth * objects) >>> 0).toString(16).toUpperCase().padStart(8, '0');
  };
  S.lampI = function (L) {
    const s = this.lamp;
    const delay = Math.max(0, L - s.origin) * 0.075;
    const p = clamp((this.t - s.t0 - delay) / 0.3, 0, 1);
    let I = s.on ? Math.min(1.25, Math.sin(p * Math.PI / 2) + 0.45 * Math.sin(p * Math.PI)) : s.from * (1 - p);
    // "insufficient, incessant": a faint, constant flicker, and once in a while a stutter
    const n = hash(Math.floor(this.t * 9), L);
    I *= 0.93 + 0.07 * ((n & 255) / 255);
    if (n % 211 === 0) I *= 0.55;
    return I;
  };
  S.lanternI = function () {
    const p = clamp((this.t - this.lanternState.t0) / 0.9, 0, 1);
    return this.lanternState.on ? easeOut(p) : 1 - p;
  };
  S.setMode = function (mode) {
    if (!ENCODINGS[mode] || ENCODINGS[mode] === this.enc) return;
    this.enc = ENCODINGS[mode];
    this.path = []; this.reveal = 0; this.landed.clear(); this.pathVersion++; this.epoch++;
    this.queue = []; this.act = null; this.flyers = [];
    if (this.host) { this.levelBase = Math.ceil(this.camZ - 1 + Z_NEXT); this.camZ = this.rest(0); }
  };

  /* ---------------------------------------------------------- actions */
  S.push = function (a) { this.queue.push(a); return this; };
  S.wait = function (d) { return this.push({ kind: 'wait', dur: d }); };
  S.connect = function () {
    return this.push({
      kind: 'connect', dur: 1,
      start: sc => {
        if (sc.host) return 0;
        sc.host = true;
        sc.lamp = { on: true, t0: sc.t + 0.3, origin: Math.floor(sc.camZ), from: 0 };
        sc.lanternState = { on: false, t0: sc.t + 0.2 };
        sc.wipe = { names: sc.path.slice(0, sc.reveal).map(c => sc.enc.name(c)), t0: sc.t, dur: 0.45 };
        sc.path = []; sc.reveal = 0; sc.landed.clear(); sc.pathVersion++; sc.epoch++;
        const z0 = sc.camZ, v = Math.max(sc.vel, 0.05);
        sc.levelBase = Math.ceil(z0 - 1 + Z_NEXT + Math.max(0.3, v * 0.7));
        const dist = sc.rest(0) - z0;
        sc._mv = { z0, dist, dur: clamp(2 * dist / v, 0.5, 1.5) };
        sc.vel = 0;
        sc.toast = { text: 'HOST CONNECTED', color: LAMP, t0: sc.t, dur: 1.6 };
        sc.emit('OpenSession', 'session 1');
        return sc._mv.dur;
      },
      update: (sc, p) => { sc.camZ = sc._mv.z0 + sc._mv.dist * (1 - (1 - p) * (1 - p)); },
    });
  };
  S.disconnect = function () {
    return this.push({
      kind: 'disconnect', dur: 1.2,
      start: sc => {
        if (!sc.host) return 0;
        if (sc.book.isOpen) sc._closeBook();
        sc.host = false;
        sc.lamp = { on: false, t0: sc.t, origin: Math.floor(sc.camZ), from: 1 };
        sc.lanternState = { on: true, t0: sc.t + 0.4 };
        sc.toast = { text: 'HOST GONE · FREEFALL', color: DUSK, t0: sc.t, dur: 1.6 };
        sc.emit('CloseSession', 'cable pulled');
      },
      update: (sc, p) => { sc.vel = FALL_SPEED * easeIn(p); },
      end: sc => { sc.vel = FALL_SPEED; },
    });
  };
  S.enter = function (comp, fast) {
    const LOCK = fast ? 0.06 : 0.16, MOVE = fast ? 0.26 : 0.46;
    return this.push({
      kind: 'enter', dur: LOCK + MOVE,
      start: sc => {
        if (sc.book.isOpen) sc._closeBook();
        const depth = sc.path.length;
        sc.path.push(comp); sc.pathVersion++;
        sc.lockFx = { L: sc.levelBase + sc.path.length, t0: sc.t };
        sc._mv = { z0: sc.camZ, z1: sc.rest(sc.path.length), lock: LOCK / (LOCK + MOVE) };
        sc.emit('GetObjectHandles', 'parent ' + sc.handleFor(depth, comp) + '  "' + sc.enc.name(comp) + '"');
      },
      update: (sc, p) => {
        const q = clamp((p - sc._mv.lock) / (1 - sc._mv.lock), 0, 1);
        sc.camZ = lerp(sc._mv.z0, sc._mv.z1, easeInOut(q));
        sc.fovKick = Math.sin(q * Math.PI);
      },
      end: sc => { sc.camZ = sc._mv.z1; sc.fovKick = 0; },
    });
  };
  S.up = function (fast) {
    const MOVE = fast ? 0.22 : 0.44;
    return this.push({
      kind: 'up', dur: MOVE,
      start: sc => {
        if (!sc.path.length) return 0;
        if (sc.book.isOpen) sc._closeBook();
        const L = sc.levelBase + sc.path.length;
        const comp = sc.path.pop(); sc.pathVersion++; sc.epoch++;
        const name = sc.enc.name(comp);
        const lay = layoutPath(sc.path.slice(0, sc.reveal).map(c => sc.enc.name(c)).concat([name]), PATH_MAX);
        const sx = lay.slotX[lay.slotX.length - 1];
        if (sx !== null && sc.reveal > sc.path.length) sc.flyers.push({ kind: 'lift', text: name, x0: sx, y0: PATH_Y, x1: sx, y1: PATH_Y - 6, t0: sc.t, dur: 0.3, arc: 0 });
        sc.reveal = Math.min(sc.reveal, sc.path.length);
        sc.landed.delete(L);
        sc.unlockFx = { L, name, t0: sc.t + MOVE };
        sc._mv = { z0: sc.camZ, z1: sc.rest(sc.path.length) };
        sc.emit('GetObjectHandles', 'parent ' + (sc.path.length ? sc.handleFor(sc.path.length - 1, sc.path[sc.path.length - 1]) : 'root') + '  (back)');
      },
      update: (sc, p) => { sc.camZ = lerp(sc._mv.z0, sc._mv.z1, easeInOut(p)); sc.fovKick = -0.6 * Math.sin(p * Math.PI); },
      end: sc => { if (sc._mv) sc.camZ = sc._mv.z1; sc.fovKick = 0; },
    });
  };
  S.root = function () {
    return this.push({
      kind: 'root', dur: 0.6,
      start: sc => {
        if (sc.book.isOpen) sc._closeBook();
        const n = sc.path.length;
        const dur = clamp(0.4 + 0.07 * n, 0.45, 1.3);
        sc.wipe = { names: sc.path.slice(0, sc.reveal).map(c => sc.enc.name(c)), t0: sc.t, dur: dur * 0.8 };
        sc.path = []; sc.reveal = 0; sc.landed.clear(); sc.pathVersion++; sc.epoch++;
        sc._mv = { z0: sc.camZ, z1: sc.rest(0) };
        sc.emit('GetObjectHandles', 'parent root (0xFFFFFFFF)');
        return dur;
      },
      update: (sc, p) => { sc.camZ = lerp(sc._mv.z0, sc._mv.z1, easeInOut(p)); sc.fovKick = -Math.sin(p * Math.PI); },
      end: sc => { sc.camZ = sc._mv.z1; sc.fovKick = 0; },
    });
  };
  S.open = function () {
    return this.push({
      kind: 'open', dur: 0.95,
      start: sc => {
        sc.book = { openAt: sc.t, closeAt: -1, isOpen: true };
        const depth = sc.path.length;
        sc.emit('GetObject', '"file" ' + '0x' + (sc.enc.objects + depth * sc.enc.objects).toString(16).toUpperCase().padStart(8, '0') + '  ' + sc.content().length + ' B');
      },
    });
  };
  S.close = function () {
    return this.push({ kind: 'close', dur: 0.75, start: sc => { if (sc.book.isOpen) sc._closeBook(); else return 0; } });
  };
  S._closeBook = function () { this.book.closeAt = this.t; this.book.isOpen = false; };
  S.navigateTo = function (target, interval, replace) {
    // plan from the path as it will be once the queue drains; a live lookup
    // (replace) drops whatever was still pending and re-plans from here
    if (replace) this.queue = this.queue.filter(a => a.kind === 'connect');
    let cur = this.path.slice();
    for (const a of this.queue) {
      if (a.kind === 'enter') cur.push(a.comp);
      else if (a.kind === 'up') cur.pop();
      else if (a.kind === 'root') cur = [];
    }
    let common = 0;
    while (common < cur.length && common < target.length && cur[common] === target[common]) common++;
    const ups = cur.length - common, downs = target.length - common;
    const fast = ups + downs > 4;
    if (ups > 2 && common === 0) this.root();
    else for (let i = 0; i < ups; i++) this.up(true);
    for (let i = common; i < target.length; i++) {
      this.enter(target[i], fast && !interval);
      this.queue[this.queue.length - 1].comp = target[i];
      if (interval && i < target.length - 1) this.wait(interval);
    }
    return this;
  };
  S.goto = function (text, interval) { return this.navigateTo(this.enc.pathFor(textBytes(text)), interval); };
  S.toggleBook = function () {
    if (this.book.isOpen) return this.close();
    if (!this.host) this.connect();
    return this.open();
  };

  /* ------------------------------------------------------------ update */
  S.update = function (dt) {
    this.t += dt;
    const sc = this;
    if (this.director) this.director(this, dt);

    // run queued actions
    if (!this.act && this.queue.length) {
      const a = this.queue.shift();
      const d = a.start ? a.start(this) : undefined;
      this.act = { a, t0: this.t, dur: d === 0 ? 0 : (typeof d === 'number' ? d : a.dur) };
    }
    if (this.act) {
      const { a, t0, dur } = this.act;
      const p = dur > 0 ? clamp((this.t - t0) / dur, 0, 1) : 1;
      if (a.update && dur > 0) a.update(this, p);
      if (p >= 1) { if (a.end && dur > 0) a.end(this); this.act = null; }
    }

    // freefall while nobody is browsing: the librarian wanders
    const moving = this.act && /connect|enter|up|root/.test(this.act.a.kind);
    if (!this.host && !moving) {
      this.camZ += this.vel * dt;
      const nextL = this.levelBase + this.path.length + 1;
      if (nextL + TAB_FV - this.camZ < Z_LOCK) {
        this.path.push(this.enc.random(this.rnd)); this.pathVersion++;
        this.lockFx = { L: nextL, t0: this.t };
      }
      if (this.path.length > 48) {
        const drop = 24;
        this.path.splice(0, drop); this.levelBase += drop;
        this.reveal = Math.max(0, this.reveal - drop); this.pathVersion++; this.epoch++;
        this.flyers = this.flyers.filter(f => f.kind !== 'land');
      }
    }

    // a tab reaching the catalog card turns into a flyer bound for its slot
    for (let i = this.reveal + 1; i <= this.path.length; i++) {
      const L = this.levelBase + i;
      if (this.landed.has(L)) continue;
      const z = L + TAB_FV - this.camZ;
      if (z >= Z_LAND) continue;
      this.landed.add(L);
      const P = this.params();
      const ap = shaftPoint(P, 1.5, Math.max(z, 0.3), [0, 0]);
      const names = this.path.slice(0, i).map(c => this.enc.name(c));
      const lay = layoutPath(names, PATH_MAX);
      const x1 = lay.slotX[i - 1] !== null ? lay.slotX[i - 1] : PATH_X + 6;
      const epoch = this.epoch, idx = i;
      this.flyers.push({
        kind: 'land', text: this.enc.name(this.path[i - 1]), x0: ap[0] - 5, y0: Math.min(ap[1] - 3, HUD_Y - 8),
        x1, y1: PATH_Y, t0: this.t, dur: 0.3, arc: 4,
        land: () => { if (sc.epoch === epoch && sc.path.length >= idx) { sc.reveal = Math.max(sc.reveal, idx); sc.revealAt = sc.t; } },
      });
    }
    this.flyers = this.flyers.filter(f => {
      if (this.t - f.t0 >= f.dur) { if (f.land) f.land(); return false; }
      return true;
    });

    // data motes drift up out of the dark
    for (const p of this.particles) {
      p.v -= p.speed * dt;
      const z = p.v - this.camZ;
      if (z < 1.2 || z > 16) Object.assign(p, this._particle(false));
    }

    // hand tremor / IMU tilt
    if (this.tiltFn) {
      const v = this.tiltFn(this.t);
      this.tilt.x = v.x; this.tilt.y = v.y; this.tilt.roll = v.roll;
    } else if (this.autoTilt) {
      this.tilt.x = 5 * Math.sin(this.t * 0.31) + 2 * Math.sin(this.t * 0.83 + 1);
      this.tilt.y = 2.5 * Math.sin(this.t * 0.23 + 1.3);
      this.tilt.roll = 0.035 * Math.sin(this.t * 0.17 + 0.4);
    }
    const k = 1 - Math.exp(-dt * 5);
    this.tiltS.x += (this.tilt.x - this.tiltS.x) * k;
    this.tiltS.y += (this.tilt.y - this.tiltS.y) * k;
    this.tiltS.roll += (this.tilt.roll - this.tiltS.roll) * k;
  };

  S.params = function () {
    const sc = this;
    const bob = this.host ? 0.035 * Math.sin(this.t * this.bobFreq) : 0;
    return {
      vpx: VP_X + this.tiltS.x, vpy: VP_Y + this.tiltS.y, roll: this.tiltS.roll,
      camZ: this.camZ + bob, K: K_BASE * (1 + 0.07 * this.fovKick),
      ambient: this.host ? 0.52 : 0.4,
      lantern: this.lanternI(),
      lampI: L => sc.lampI(L),
    };
  };

  S.tabInfo = function (L) {
    const i = L - this.levelBase;
    if (i < 1) return null;
    const u = this.unlockFx;
    if (u && u.L === L && this.t - u.t0 < 0.5 && i > this.path.length) return { text: u.name, style: 'known' };
    if (i <= this.path.length) {
      const f = this.lockFx;
      const flash = f && f.L === L && this.t - f.t0 < 0.1;
      return { text: this.enc.name(this.path[i - 1]), style: flash ? 'flash' : 'known' };
    }
    const tick = Math.floor(this.t * 16);
    return { text: this.enc.name(this.enc.random(rng(hash(tick, L) + 1))), style: i === this.path.length + 1 ? 'flicker' : 'flicker2' };
  };

  /* ------------------------------------------------------------ render */
  S.render = function (fb) {
    const P = this.params();
    renderShaft(fb, P);
    drawFarRings(fb, P);
    drawLamps(fb, P);
    drawParticles(fb, P, this.particles, this.t);

    const bookUp = this.book.openAt >= 0 && (this.book.isOpen || this.t - this.book.closeAt < 0.75);
    if (!bookUp) {
      const bob = Math.sin(this.t * 1.7) * 1.2;
      drawSmallBook(fb, P.vpx, P.vpy + bob, this.host, this.t);
    }

    // folder tabs on the bottom railing of the next floors, nearest first;
    // a tab that would collide with the one below it waits its turn
    let lastTop = 1e9;
    for (let L = Math.floor(P.camZ) + 1; L < P.camZ + Z_TAB_MAX + 1; L++) {
      const z = L + TAB_FV - P.camZ;
      if (z < Z_LAND || z > Z_TAB_MAX || this.landed.has(L)) continue;
      const info = this.tabInfo(L);
      if (!info) continue;
      const pt = shaftPoint(P, 1.5, z, [0, 0]);
      const ty = Math.min(pt[1] - 1, HUD_Y - 7);
      if (ty + 8 > lastTop) continue;
      const a = clamp((Z_TAB_MAX - z) / 0.4, 0, 1) * (info.style === 'flicker2' ? 0.55 : 1);
      drawTab(fb, pt[0], ty, info.text, info.style, a);
      lastTop = ty - 7;
    }

    if (bookUp) drawOpenBook(fb, this, P);
    drawHUD(fb, this, P);
    drawFlyers(fb, this);
    drawTopBar(fb, this);
  };

  /* ---------------------------------------------------------- director */
  // The looping demo: freefall, a host connects and browses to "Hello",
  // reads the file, corrects it to "Help", returns to the root, unplugs.
  function demoDirector(script, opts) {
    const loop = !opts || opts.loop !== false;
    let i = 0, waitUntil = 0;
    const director = function (sc) {
      if (director.done || sc.t < waitUntil || sc.queue.length || sc.act) return;
      if (i >= script.length) {
        if (!loop) { director.done = true; return; }
        i = 0;
      }
      director.step = i;
      const [cmd, a, b] = script[i++];
      switch (cmd) {
        case 'wait': waitUntil = sc.t + a; break;
        case 'connect': sc.connect(); break;
        case 'disconnect': sc.disconnect(); break;
        case 'goto': sc.goto(a, b); break;
        case 'open': sc.open(); break;
        case 'close': sc.close(); break;
        case 'up': sc.up(); break;
        case 'root': sc.root(); break;
      }
    };
    director.done = false;
    director.step = 0;
    return director;
  }
  const DEMO_SCRIPT = [
    ['wait', 2.4],
    ['connect'], ['wait', 0.5],
    ['goto', 'Hello', 0.32], ['wait', 0.5],
    ['open'], ['wait', 2.2], ['close'], ['wait', 0.5],
    ['up'], ['wait', 0.45],
    ['goto', 'Help', 0.3], ['wait', 0.4],
    ['open'], ['wait', 1.5], ['close'], ['wait', 0.5],
    ['root'], ['wait', 0.9],
    ['disconnect'], ['wait', 2.2],
  ];

  // Design-review overlay: the geometry the layout is built on.
  function drawGuides(fb, sc) {
    const P = sc.params();
    for (let i = -7; i <= 7; i++) { if (i & 1) continue; pset(fb, P.vpx + i, P.vpy, CRIMSON); pset(fb, P.vpx, P.vpy + i, CRIMSON); }
    hexOutline(fb, P.vpx, P.vpy, P.K / Z_NEXT, P.roll, CYAN, 0.5);
    hexOutline(fb, P.vpx, P.vpy, P.K / Z_LAND, P.roll, CRIMSON, 0.5);
    for (let x = 0; x < SW; x += 2) { pset(fb, x, HUD_Y, CRIMSON); pset(fb, x, 10, CRIMSON); }
    for (let y = 0; y < SH; y += 2) { pset(fb, 4, y, DUSK); pset(fb, SW - 4, y, DUSK); }
  }
  const LAYOUT = { VP_X, VP_Y, K: K_BASE, HUD_Y, Z_NEXT, Z_LAND, LAMP_FV, TAB_FV, PATH_X, PATH_Y, BODY_Y, FALL_SPEED, F, TW };
  const RAMP_NAMES = ['Shadow', 'Railing', 'Wood', 'Oxblood cloth', 'Moss cloth', 'Indigo cloth', 'Gilt', 'Vellum'];

  const api = {
    SW, SH, PALETTE, ENCODINGS, ALPHA70, FONT5_SRC, FONT3_SRC, LAYOUT, RAMPS, RAMP_NAMES, NOM,
    Scene, demoDirector, DEMO_SCRIPT, textBytes, drawGuides,
    createFrame: () => new Uint8Array(SW * SH),
    // debug hooks for the exporters
    _internal: { renderShaft, layoutPath, text5, text3, fillRect, drawTab },
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.BabelScreen = api;
})(typeof window !== 'undefined' ? window : globalThis);
