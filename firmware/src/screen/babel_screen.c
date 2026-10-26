/*
 * babel_screen.c  ·  "The Air Shaft"
 * ---------------------------------------------------------------------------
 * C port of design/babel-screen.js. Section order, names and timings follow
 * the JS file so the two can be read side by side; where the port differs it
 * says so. Single-precision float throughout (the ESP32-S3 FPU has no double).
 *
 *   "The universe (which others call the Library) is composed of an
 *    indefinite, perhaps infinite number of hexagonal galleries, with vast
 *    air shafts between, surrounded by very low railings."     (Borges)
 */
#include "babel_screen.h"
#include "bs_tables.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW BS_W
#define SH BS_H
#define PI_F 3.14159265f

enum { VOID = BS_VOID, INK, SHADE, DUSK, UMBER, WOOD, OXBLOOD, MOSS, INDIGO, OCHRE, VELLUM, LAMP, EMBER, CYAN, WHITE, CRIMSON };

// One step darker for every colour: used to sink the shaft behind the open book.
static const uint8_t DARKER[16] = { VOID, VOID, INK, SHADE, INK, UMBER, UMBER, SHADE, SHADE, WOOD, DUSK, EMBER, UMBER, DUSK, VELLUM, OXBLOOD };

/* ========================================================= dither, noise */
static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static float BAYERF[16];
static inline float bayer(int x, int y) { return BAYERF[((y & 3) << 2) | (x & 3)]; }

static inline uint32_t xorshift(uint32_t *s) {
  uint32_t v = *s;
  v ^= v << 13; v ^= v >> 17; v ^= v << 5;
  return *s = v;
}
static inline uint32_t rng_seed(uint32_t seed) { return seed ? seed : 0x9e3779b9u; }
static inline double rnd_d(uint32_t *s) { return xorshift(s) / 4294967296.0; }
static inline float rnd_f(uint32_t *s) { return (xorshift(s) >> 8) * (1.0f / 16777216.0f); }

static uint32_t hash2(int a, int b) {
  uint32_t h = ((uint32_t)a * 0x27d4eb2du) ^ (((uint32_t)b + 0x632be5abu) * 0x165667b1u);
  h = (h ^ (h >> 15)) * 0x85ebca6bu;
  h = (h ^ (h >> 13)) * 0xc2b2ae35u;
  return h ^ (h >> 16);
}
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float ease_in_out(float t) { return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
static inline float ease_out(float t) { float u = 1 - t; return 1 - u * u * u; }
static inline float ease_in(float t) { return t * t; }
static inline int iround(float v) { return (int)lroundf(v); }

/* ============================================================ primitives */
static inline void pset(uint8_t *fb, int x, int y, uint8_t c) {
  if ((unsigned)x < SW && (unsigned)y < SH) fb[y * SW + x] = c;
}
// Coverage-dithered pixel: a is 0..1 opacity, resolved by the Bayer matrix.
static inline void dset(uint8_t *fb, int x, int y, uint8_t c, float a) {
  if ((unsigned)x < SW && (unsigned)y < SH && (a >= 1 || bayer(x, y) < a)) fb[y * SW + x] = c;
}
static void fill_rect(uint8_t *fb, float x, float y, float w, float h, uint8_t c, float a) {
  int x0 = iround(x), y0 = iround(y), x1 = iround(x + w), y1 = iround(y + h);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > SW) x1 = SW;
  if (y1 > SH) y1 = SH;
  if (x1 <= x0 || y1 <= y0) return;
  if (a >= 1) { for (int j = y0; j < y1; j++) memset(fb + j * SW + x0, c, x1 - x0); return; }
  for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) if (bayer(i, j) < a) fb[j * SW + i] = c;
}
static void darken_rect(uint8_t *fb, float x, float y, float w, float h, float amount) {
  int x0 = iround(x), y0 = iround(y), x1 = iround(x + w), y1 = iround(y + h);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > SW) x1 = SW;
  if (y1 > SH) y1 = SH;
  int whole = (int)floorf(amount);
  float frac = amount - whole;
  for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
    int n = whole + (bayer(i, j) < frac ? 1 : 0);
    uint8_t c = fb[j * SW + i];
    while (n-- > 0) c = DARKER[c];
    fb[j * SW + i] = c;
  }
}
static void line(uint8_t *fb, float fx0, float fy0, float fx1, float fy1, uint8_t c, float a) {
  int x0 = iround(fx0), y0 = iround(fy0), x1 = iround(fx1), y1 = iround(fy1);
  int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int guard = 0; guard < 2000; guard++) {
    dset(fb, x0, y0, c, a);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}
static void disc(uint8_t *fb, float cx, float cy, float r, uint8_t c, float a) {
  float r2 = r * r;
  for (int y = (int)floorf(cy - r); y <= (int)ceilf(cy + r); y++)
    for (int x = (int)floorf(cx - r); x <= (int)ceilf(cx + r); x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      if (dx * dx + dy * dy <= r2) dset(fb, x, y, c, a);
    }
}
// Soft radial glow: dithered coverage falling off as (1 - d)^1.6, from a table.
#define GLOW_N 512
static float GLOW[GLOW_N];
static void glow(uint8_t *fb, float cx, float cy, float r, uint8_t c, float a) {
  if (a <= 0.01f || r < 0.5f) return;
  float inv = (GLOW_N - 1) / (r * r);
  int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r), x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
  if (y0 < 0) y0 = 0;
  if (x0 < 0) x0 = 0;
  if (y1 >= SH) y1 = SH - 1;
  if (x1 >= SW) x1 = SW - 1;
  for (int y = y0; y <= y1; y++) {
    float dy = y + 0.5f - cy;
    for (int x = x0; x <= x1; x++) {
      float dx = x + 0.5f - cx, d2 = (dx * dx + dy * dy) * inv;
      if (d2 < GLOW_N - 1) {
        float cov = a * GLOW[(int)d2];
        if (cov >= 1 || bayer(x, y) < cov) fb[y * SW + x] = c;
      }
    }
  }
}
// Flat-top hexagon outline (vertices left and right), the shape of every gallery.
static void hex_outline(uint8_t *fb, float cx, float cy, float R, float roll, uint8_t c, float a) {
  float px = 0, py = 0;
  for (int j = 0; j <= 6; j++) {
    float ang = j * PI_F / 3 + roll;
    float x = cx + R * cosf(ang), y = cy + R * sinf(ang);
    if (j) line(fb, px, py, x, y, c, a);
    px = x; py = y;
  }
}

/* ================================================================== text */
static inline int glyph_index(uint8_t ch) {
  if (ch >= 32 && ch < 127) return ch - 32;
  if (ch >= 0x80 && ch < 0x80 + (BS_GLYPHS - 95)) return 95 + (ch - 0x80);
  return '?' - 32;
}
static int raw_char5(uint8_t *fb, uint8_t ch, int x, int y, uint8_t c, float a) {
  const uint8_t *g = BS_FONT5[glyph_index(ch)];
  for (int r = 0; r < 8; r++) {
    uint8_t m = g[r];
    if (m) for (int i = 0; i < 5; i++) if (m & (16 >> i)) dset(fb, x + i, y + r, c, a);
  }
  return x + 6;
}
static int raw_text5(uint8_t *fb, const char *s, float fx, float fy, uint8_t c, float a) {
  int x = iround(fx), y = iround(fy);
  for (; *s; s++) x = raw_char5(fb, (uint8_t)*s, x, y, c, a);
  return x;
}
// Text with a 1px dark keyline so it reads over the shaft (outline < 0: none).
static int text5(uint8_t *fb, const char *s, float x, float y, uint8_t c, int outline, float a) {
  if (outline >= 0)
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
      if (dx || dy) raw_text5(fb, s, x + dx, y + dy, (uint8_t)outline, a);
  return raw_text5(fb, s, x, y, c, a);
}
static int text3(uint8_t *fb, const char *s, float fx, float fy, uint8_t c, float a) {
  int x = iround(fx), y = iround(fy);
  for (; *s; s++) {
    const uint8_t *g = BS_FONT3[glyph_index((uint8_t)*s)];
    for (int r = 0; r < 5; r++) for (int i = 0; i < 3; i++)
      if (g[r] & (4 >> i)) dset(fb, x + i, y + r, c, a);
    x += 4;
  }
  return x;
}
static int w5(const char *s) { return (int)strlen(s) * 6 - 1; }
static int w3(const char *s) { return (int)strlen(s) * 4 - 1; }
static void big_glyph(uint8_t *fb, uint8_t ch, int x, int y, uint8_t c, int s, float a) {
  const uint8_t *g = BS_FONT5[glyph_index(ch)];
  for (int r = 0; r < 8; r++) for (int i = 0; i < 5; i++)
    if (g[r] & (16 >> i)) for (int dy = 0; dy < s; dy++) for (int dx = 0; dx < s; dx++) dset(fb, x + i * s + dx, y + r * s + dy, c, a);
}

/* ============================================================ encodings */
// HEX:  256 folders per level named 00..FF, one byte per level.
// B70:  p2r3/babel-usb as shipped: 70-symbol alphabet, two-character names,
//       70^2 = 4900 folders + 1 `file` per directory, path read as a bijective
//       base-4900 number and written out as bijective base-256 bytes.
static const char ALPHA70[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789! &()-_+";
static const char HEXD[] = "0123456789ABCDEF";

int bs_fanout(int mode) { return mode == BS_MODE_HEX ? 256 : 4900; }

void bs_name(int mode, uint16_t comp, char out[3]) {
  if (mode == BS_MODE_HEX) { out[0] = HEXD[(comp >> 4) & 15]; out[1] = HEXD[comp & 15]; }
  else { out[0] = ALPHA70[(comp / 70) % 70]; out[1] = ALPHA70[comp % 70]; }
  out[2] = 0;
}

int bs_content_of(int mode, const uint16_t *path, int n, uint8_t *out, int max) {
  if (mode == BS_MODE_HEX) {
    int len = n < max ? n : max;
    for (int i = 0; i < len; i++) out[i] = (uint8_t)path[i];
    return len;
  }
  // N = sum (path[i] + 1) * 4900^(n-1-i), built little-endian in `out`...
  int len = 0;
  for (int i = 0; i < n; i++) {
    uint32_t carry = path[i] + 1u;
    for (int j = 0; j < len; j++) {
      uint32_t v = out[j] * 4900u + carry;
      out[j] = v & 255; carry = v >> 8;
    }
    while (carry && len < max) { out[len++] = carry & 255; carry >>= 8; }
    if (carry) break;   // the file outgrew `out`: stop at the deepest folder that fits
  }
  // ...then rewritten in place as bijective base 256: repeatedly N -= 1,
  // emit N % 256, N /= 256. Each step consumes one byte and emits one.
  int lo = 0;
  while (lo < len) {
    int j = lo;
    while (j < len && out[j] == 0) out[j++] = 255;   // borrow
    if (j < len) out[j]--;
    lo++;
    while (len > lo && out[len - 1] == 0) len--;
  }
  return lo;
}

int bs_path_for(int mode, const uint8_t *bytes, int len, uint16_t *out, int max) {
  if (mode == BS_MODE_HEX) {
    int n = len < max ? len : max;
    for (int i = 0; i < n; i++) out[i] = bytes[i];
    return n;
  }
  // N = sum (bytes[i] + 1) * 256^i, stored big-endian base 256 for long division
  static uint8_t num[BS_CONTENT_MAX + 2];
  if (len > BS_CONTENT_MAX) len = BS_CONTENT_MAX;
  int nl = len + 1;
  memset(num, 0, nl);
  for (int i = 0; i < len; i++) {
    // add (bytes[i] + 1) at little-endian position i == big-endian index nl-1-i
    int k = nl - 1 - i;
    uint32_t carry = bytes[i] + 1u;
    while (carry && k >= 0) { uint32_t v = num[k] + carry; num[k] = v & 255; carry = v >> 8; k--; }
  }
  int start = 0, n = 0;
  uint16_t tmp[BS_PATH_MAX];
  for (;;) {
    while (start < nl && num[start] == 0) start++;
    if (start >= nl) break;
    // N -= 1
    int k = nl - 1;
    while (num[k] == 0) num[k--] = 255;
    num[k]--;
    // N /= 4900, keep the remainder
    uint32_t rem = 0;
    for (int j = start; j < nl; j++) { uint32_t v = (rem << 8) | num[j]; num[j] = (uint8_t)(v / 4900); rem = v % 4900; }
    if (n < BS_PATH_MAX) tmp[n++] = (uint16_t)rem;
  }
  int m = n < max ? n : max;
  for (int i = 0; i < m; i++) out[i] = tmp[n - 1 - i];
  return m;
}

/* ============================================================ the shaft */
// Texture space: u runs around the hexagon (6 walls x TW texels), v runs down
// the shaft (F texels per level). One level = railing, floor slab, then five
// shelves of books ("twenty shelves, five long shelves per side").
#define F 48
#define TW 48
#define LW (TW * 6)
#define LHALF 72                  // light maps fold: period 3 walls, mirrored
static const float RAIL_FV = 1.0f / F, LAMP_FV = 3.0f / F, TAB_FV = 1.0f / F;
static const float K_BASE = 140;  // ring radius in px at a depth of one level
static const float VP_X = 120, VP_Y = 52;
#define HUD_Y 111
static const float FALL_SPEED = 0.55f;
static const float Z_FOG0 = 2.8f, Z_FOG1 = 12;
static const float Z_NEXT = 3.3f;
static const float Z_LAND = 2.75f;
static const float Z_LOCK = 3.7f;
static const float Z_TAB_MAX = 3.72f;
static inline float fog(float z) {
  if (z <= Z_FOG0) return 1;
  if (z >= Z_FOG1) return 0;
  float q = (Z_FOG1 - z) / (Z_FOG1 - Z_FOG0);
  return q * q;
}
// The falling reader's own lantern: lights the near walls, never the depths.
#define LANT_N 256
static float LANT[LANT_N];
static inline float lantern(float z) {
  if (z <= 1) return 0.8f;
  float i = (z - 1) * ((LANT_N - 1) / 3.4f);
  return i >= LANT_N - 1 ? 0 : LANT[(int)i];
}

enum { R_DARK, R_RAIL, R_WOOD, R_RED, R_GREEN, R_BLUE, R_GOLD, R_PAPER, R_COUNT };
static const uint8_t RAMP_LEN[R_COUNT] = { 3, 7, 6, 5, 5, 5, 6, 5 };
static const uint8_t RAMPS[R_COUNT][8] = {
  { VOID, INK, SHADE },
  { VOID, INK, SHADE, UMBER, WOOD, EMBER, LAMP },
  { VOID, INK, SHADE, UMBER, WOOD, EMBER },
  { VOID, INK, SHADE, OXBLOOD, EMBER },
  { VOID, INK, SHADE, MOSS, OCHRE },
  { VOID, INK, SHADE, DUSK, INDIGO },
  { VOID, INK, UMBER, WOOD, OCHRE, LAMP },
  { VOID, INK, SHADE, DUSK, VELLUM },
};
static const uint8_t NOM[R_COUNT] = { 2, 4, 4, 3, 3, 4, 4, 3 };
static float nb(int r) { return (float)NOM[r] / (RAMP_LEN[r] - 1); }
static const uint8_t BOOK_RAMPS[13] = { R_RED, R_RED, R_RED, R_RED, R_GREEN, R_GREEN, R_BLUE, R_BLUE, R_BLUE, R_GOLD, R_WOOD, R_WOOD, R_PAPER };

// Textures live in RAM: per texel a nibble of ramp and a byte of brightness
// (bri * 200). 20 walls x 5 mips x (48^2 + 24^2 + 12^2 + 6^2 + 3^2) texels.
#define NVAR 16
#define NDOOR 4
#define NTEX (NVAR + NDOOR)
#define TEX_TEXELS (48 * 48 + 24 * 24 + 12 * 12 + 6 * 6 + 3 * 3)
#define BRI_SCALE 200.0f
typedef struct { uint8_t rows, cols; uint16_t off; const uint8_t *ramp, *bri; } mip_t;   // ramp: nibbles, indexed from off
typedef struct { mip_t m[5]; } tex_t;
static uint8_t TEX_RAMP[NTEX][(TEX_TEXELS + 1) / 2];
static uint8_t TEX_BRI[NTEX][TEX_TEXELS];
static tex_t TEX[NTEX];
// brightness byte -> position along the ramp at light 1
static float BRI_STEP[R_COUNT];

static void gen_wall(uint32_t seed, bool door, uint8_t *ramp, float *bri) {
  uint32_t s = rng_seed(seed);
#define PUT(r, c, rp, b) do { ramp[(r) * TW + (c)] = (rp); bri[(r) * TW + (c)] = (b); } while (0)
  for (int c = 0; c < TW; c++) {
    PUT(0, c, R_RAIL, nb(R_RAIL) * 1.15f);                    // top rail, catches the lamps
    PUT(1, c, R_RAIL, nb(R_RAIL) * 0.8f);
    bool post = (c & 3) == 1;                                 // balusters; the gallery floor shows between
    for (int r = 2; r <= 4; r++) PUT(r, c, R_WOOD, post ? nb(R_WOOD) * (r == 2 ? 1 : 0.85f) : nb(R_WOOD) * 0.42f);
    PUT(5, c, R_WOOD, nb(R_WOOD) * 0.95f);                    // slab edge
    PUT(6, c, R_WOOD, nb(R_WOOD) * 0.68f);
    PUT(7, c, R_DARK, 0.45f);
  }
  static const int WIDTHS[6] = { 2, 2, 3, 3, 3, 4 };
  for (int sh = 0; sh < 5; sh++) {
    int top = 8 + sh * 8;
    for (int c = 0; c < TW; c++) {
      PUT(top, c, R_DARK, 0.22f);
      for (int r = top + 1; r < top + 7; r++) PUT(r, c, R_DARK, 0.5f);
      PUT(top + 7, c, R_WOOD, nb(R_WOOD) * 0.9f);              // the shelf board
    }
    int c = 0;
    while (c < TW) {
      if (rnd_d(&s) < 0.07) { c += 1 + (int)(rnd_d(&s) * 2); continue; }
      int w = WIDTHS[(int)(rnd_d(&s) * 6)];
      int h = 4 + (int)(rnd_d(&s) * 3);
      int rp = BOOK_RAMPS[(int)(rnd_d(&s) * 13)];
      float tone = (float)(0.86 + rnd_d(&s) * 0.28);
      bool gilt = rnd_d(&s) < 0.6;
      int g_row = top + 7 - h + 1 + (int)(rnd_d(&s) * 2);
      for (int i = 0; i < w && c + i < TW; i++) {
        for (int r = top + 7 - h; r < top + 7; r++) {
          if (gilt && r == g_row && i > 0) PUT(r, c + i, R_GOLD, nb(R_GOLD) * 0.95f);
          else PUT(r, c + i, rp, nb(rp) * tone * (i == 0 ? 0.68f : 1));
        }
      }
      c += w;
    }
  }
  if (door) {
    // One of the two free sides: the hallway to the next gallery.
    for (int r = 8; r < F; r++) for (int c = 9; c <= 22; c++) PUT(r, c, R_DARK, 0.06f);
    for (int r = 8; r < F; r++) { PUT(r, 8, R_WOOD, nb(R_WOOD) * 0.7f); PUT(r, 23, R_WOOD, nb(R_WOOD) * 0.7f); }
    for (int c = 8; c <= 23; c++) { PUT(8, c, R_WOOD, nb(R_WOOD) * 0.85f); PUT(9, c, R_WOOD, nb(R_WOOD) * 0.55f); }
  }
#undef PUT
}
// Box-filtered mips: the dominant material wins, brightness is averaged.
static void build_tex(int t, uint32_t seed, bool door, uint8_t *base_ramp, float *base_bri) {
  gen_wall(seed, door, base_ramp, base_bri);
  uint8_t *ramp = TEX_RAMP[t], *bri = TEX_BRI[t];
  int off = 0;
  for (int m = 0; m <= 4; m++) {
    int s = 1 << m, rows = F >> m, cols = TW >> m;
    mip_t *mp = &TEX[t].m[m];
    mp->rows = (uint8_t)rows; mp->cols = (uint8_t)cols; mp->off = (uint16_t)off; mp->ramp = ramp; mp->bri = bri + off;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
      float weight[R_COUNT] = { 0 }, sum = 0;
      for (int i = 0; i < s; i++) for (int j = 0; j < s; j++) {
        int idx = (r * s + i) * TW + (c * s + j);
        weight[base_ramp[idx]] += base_bri[idx] + 0.02f;
        sum += base_bri[idx];
      }
      int best = 0;
      for (int k = 1; k < R_COUNT; k++) if (weight[k] > weight[best]) best = k;
      float b = sum / (s * s) * BRI_SCALE + 0.5f;
      int n = off + r * cols + c;
      ramp[n >> 1] |= (uint8_t)(best << ((n & 1) * 4));
      bri[off + r * cols + c] = (uint8_t)(b > 255 ? 255 : b);
    }
    off += rows * cols;
  }
}
static inline const tex_t *wall_tex(int L, int k) {
  if (k == 1 || k == 4) return &TEX[NVAR + hash2(L, k) % NDOOR];   // bottom and top walls are the free sides
  return &TEX[hash2(L, k) % NVAR];
}

// Light maps: "two spherical lamps per hexagon, placed transversally". The
// lamps sit on the left and right corners of each railing. The same level, the
// level above and the level below are kept apart so each floor's lamps can
// ignite on their own. Port note: the lamps at u = 0 and u = 3 make the map
// repeat every 3 walls and mirror within that, so only 72 of 288 columns are kept.
static uint16_t LM[F][LHALF][3];   // light * LM_SCALE
#define LM_SCALE 16384.0f
static uint8_t LMCOL[LW];

// Lens vignette, fixed to the glass: 1 - 0.34 * min(1.3, r2)^1.25, from a table on r2.
#define VIG_N 1024
static float VIG_LUT[VIG_N + 1];
static float VX2[SW], VY2[SH];

static const float TANG[6][2] = { { -0.5f, 0.8660254f }, { -1, 0 }, { -0.5f, -0.8660254f }, { 0.5f, -0.8660254f }, { 1, 0 }, { 0.5f, 0.8660254f } };

void bs_init_tables(void) {
  static bool done = false;
  if (done) return;
  done = true;
  for (int i = 0; i < 16; i++) BAYERF[i] = (BAYER[i] + 0.5f) / 16;
  for (int i = 0; i < GLOW_N; i++) GLOW[i] = powf(1 - sqrtf((i + 0.5f) / GLOW_N), 1.6f);
  for (int i = 0; i < LANT_N; i++) {
    float z = 1 + i * (3.4f / (LANT_N - 1)), q = 1 - (z - 1) / 3.4f;
    LANT[i] = q <= 0 ? 0 : 0.8f * powf(fminf(q, 1), 1.4f);
  }
  for (int r = 0; r < R_COUNT; r++) BRI_STEP[r] = (RAMP_LEN[r] - 1) / BRI_SCALE / LM_SCALE;

  uint8_t *base_ramp = malloc(F * TW);
  float *base_bri = malloc(F * TW * sizeof(float));
  for (int i = 0; i < NVAR; i++) build_tex(i, 0x51ab + i * 7919, false, base_ramp, base_bri);
  for (int i = 0; i < NDOOR; i++) build_tex(NVAR + i, 0x7a11 + i * 104729, true, base_ramp, base_bri);
  free(base_ramp);
  free(base_bri);

  const float sig2 = 2 * 0.34f * 0.34f, LEVEL_H = 1.25f;
  for (int r = 0; r < F; r++) {
    float fv = (r + 0.5f) / F;
    for (int col = 0; col < LHALF; col++) {
      float u = (col + 0.5f) / TW;
      float acc[3] = { 0, 0, 0 };
      for (int l = 0; l < 2; l++) {
        float du = fabsf(u - l * 3);
        if (du > 3) du = 6 - du;
        for (int dl = -1; dl <= 1; dl++) {
          float dv = (fv - (LAMP_FV + dl)) * LEVEL_H, d2 = du * du + dv * dv;
          acc[dl + 1] += 0.62f * expf(-d2 / sig2) + 0.12f / (1 + 5 * d2);
        }
      }
      LM[r][col][0] = (uint16_t)(acc[1] * LM_SCALE + 0.5f);   // same level
      LM[r][col][1] = (uint16_t)(acc[0] * LM_SCALE + 0.5f);   // level above
      LM[r][col][2] = (uint16_t)(acc[2] * LM_SCALE + 0.5f);   // level below
    }
  }
  for (int c = 0; c < LW; c++) { int f = c % 144; LMCOL[c] = (uint8_t)(f >= LHALF ? 143 - f : f); }

  for (int i = 0; i <= VIG_N; i++) VIG_LUT[i] = 1 - 0.34f * powf(1.3f * i / VIG_N, 1.25f);
  for (int x = 0; x < SW; x++) { float d = (x + 0.5f - 120) / 120; VX2[x] = d * d; }
  for (int y = 0; y < SH; y++) { float d = (y + 0.5f - 58) / 70; VY2[y] = d * d; }
}

static float lamp_i(const bs_scene_t *sc, int L);

void bs_render_shaft_rows(const bs_params_t *P, uint8_t *fb, int y_begin, int y_end) {
  const float KA = P->K * 0.8660254f, cr = cosf(P->roll), sr = sinf(P->roll);
  const float FKA = F * KA, FU = TW * 0.8660254f, VSC = VIG_N / 1.3f;
  int cacheL = INT_MIN;
  float i0 = 0, iu = 0, id = 0;
  const float amb = P->ambient * LM_SCALE, lant = P->lantern * LM_SCALE;
  const tex_t *wt[6] = { 0 };
  for (int y = y_begin; y < y_end; y++) {
    const float dy0 = y + 0.5f - P->vpy, vy2 = VY2[y];
    uint8_t *row = fb + y * SW;
    for (int x = 0; x < SW; x++) {
      const float dx0 = x + 0.5f - P->vpx;
      const float rx = dx0 * cr + dy0 * sr, ry = -dx0 * sr + dy0 * cr;
      const float p1 = 0.8660254f * rx + 0.5f * ry, p2 = -0.8660254f * rx + 0.5f * ry;
      float a = p1;
      int k = 0;
      if (ry > a) { a = ry; k = 1; }
      if (p2 > a) { a = p2; k = 2; }
      if (-p1 > a) { a = -p1; k = 3; }
      if (-ry > a) { a = -ry; k = 4; }
      if (-p2 > a) { a = -p2; k = 5; }
      if (a < 0.35f) a = 0.35f;
      const float inv = 1.0f / a;
      const float z = KA * inv;                 // depth below the camera, in levels
      if (z >= Z_FOG1) { row[x] = VOID; continue; }
      const float fz = fog(z);
      float t = ((rx * TANG[k][0] + ry * TANG[k][1]) * inv * 1.7320508f + 1) * 0.5f;
      t = t < 0 ? 0 : t > 0.99999f ? 0.99999f : t;
      const float v = P->cam_z + z;
      const int L = (int)floorf(v);
      const float fv = v - L;
      if (L != cacheL) {
        cacheL = L;
        i0 = lamp_i(P->sc, L); iu = lamp_i(P->sc, L - 1); id = lamp_i(P->sc, L + 1);
        for (int w = 0; w < 6; w++) wt[w] = wall_tex(L, w);
      }
      const float footV = FKA * inv * inv, footU = FU * inv;
      const float foot = (footV > footU ? footV : footU) * 0.8f;
      const mip_t *mip = &wt[k]->m[foot < 2 ? 0 : foot < 4 ? 1 : foot < 8 ? 2 : foot < 16 ? 3 : 4];
      const int ti = (int)(fv * mip->rows) * mip->cols + (int)(t * mip->cols);
      const int tn = mip->off + ti;
      const uint16_t *lm = LM[(int)(fv * F)][LMCOL[(int)((k + t) * TW)]];
      const float light = amb + i0 * lm[0] + iu * lm[1] + id * lm[2] + lant * lantern(z);
      float r2 = VX2[x] + vy2;
      const float vig = r2 >= 1.3f ? VIG_LUT[VIG_N] : VIG_LUT[(int)(r2 * VSC)];
      const int rp = (mip->ramp[tn >> 1] >> ((tn & 1) * 4)) & 15, n = RAMP_LEN[rp] - 1;
      float pos = mip->bri[ti] * BRI_STEP[rp] * light * fz * vig;
      if (pos > n) pos = (float)n;
      const int ip = (int)pos;
      row[x] = RAMPS[rp][(pos - ip) > bayer(x, y) && ip < n ? ip + 1 : ip];
    }
  }
}

// Screen position of a point on the hexagon (u in wall units 0..6) at depth z.
static void shaft_point(const bs_params_t *P, float u, float z, float out[2]) {
  float R = P->K / z;
  int k = ((int)floorf(u)) % 6;
  float t = u - floorf(u);
  float a0 = k * PI_F / 3, a1 = (k + 1) * PI_F / 3;
  float x = R * lerpf(cosf(a0), cosf(a1), t), y = R * lerpf(sinf(a0), sinf(a1), t);
  float cr = cosf(P->roll), sr = sinf(P->roll);
  out[0] = P->vpx + x * cr - y * sr;
  out[1] = P->vpy + x * sr + y * cr;
}

/* ======================================================= shaft overlays */
// Far railings drawn as crisp hexagons so the infinite repeat stays legible
// after the texture has dissolved into fog.
static void draw_far_rings(uint8_t *fb, const bs_params_t *P) {
  for (int L = (int)floorf(P->cam_z) + 1; L < P->cam_z + 26; L++) {
    float z = L + RAIL_FV - P->cam_z;
    if (z < 10) continue;
    float lit = lamp_i(P->sc, L);
    float f = clampf(1 - (z - 10) / 16, 0, 1) * clampf((z - 10) / 2, 0, 1);
    hex_outline(fb, P->vpx, P->vpy, P->K / z, P->roll, lit > 0.5f ? UMBER : SHADE, f * 0.55f);
  }
}

static void draw_lamps(uint8_t *fb, const bs_params_t *P) {
  float pt[2];
  for (int L = (int)floorf(P->cam_z); L < P->cam_z + 12; L++) {
    float I = lamp_i(P->sc, L);
    if (I < 0.03f) continue;
    float z = L + LAMP_FV - P->cam_z;
    if (z < 0.9f) continue;
    float f = fog(z) * I;
    float r = clampf(5.2f / z, 0.45f, 4.2f);
    for (int u = 0; u <= 3; u += 3) {
      shaft_point(P, (float)u, z, pt);
      float x = pt[0], y = pt[1];
      if (x < -20 || x > SW + 20 || y < -20 || y > SH + 20) continue;
      glow(fb, x, y, r * 3.2f, EMBER, 0.42f * f);
      glow(fb, x, y, r * 1.8f, LAMP, 0.6f * f);
      if (r >= 0.9f) disc(fb, x, y, r * 0.8f, LAMP, fminf(1, f * 1.4f));
      else dset(fb, (int)x, (int)y, LAMP, f * 1.5f);
      if (r > 1.7f) disc(fb, x, y, r * 0.38f, WHITE, fminf(1, f * 1.6f));
    }
  }
}

static void draw_particles(uint8_t *fb, const bs_params_t *P, const bs_particle_t *parts, float t) {
  for (int i = 0; i < BS_PARTICLES; i++) {
    const bs_particle_t *p = &parts[i];
    float z = p->v - P->cam_z;
    if (z < 1.2f || z > 15) continue;
    float R = (P->K / z) * p->rho, ang = p->phi + P->roll;
    float x = P->vpx + R * cosf(ang), y = P->vpy + R * sinf(ang);
    float near = clampf((z - 1.2f) / 0.8f, 0, 1);
    float a = fog(z) * near * p->a;
    if (a < 0.05f) continue;
    char ch[2] = { HEXD[(p->g + (int)floorf(t * p->flip)) & 15], 0 };
    text3(fb, ch, x - 1, y - 2, z < 4.5f ? CYAN : DUSK, fminf(1, a * 1.3f));
  }
}

// The book at the bottom of the shaft: a closed volume lying face up,
// oxblood cloth, gilt frame, a hexagon stamped on the cover.
static const char *const SMALL_BOOK[8] = {
  ".kRRRRRRRR.", "kkRgggggggR", "kkRgRRhRRgR", "kkRgRhRhRgR", "kkRgRRhRRgR", "kkRgggggggR", "kkRRRRRRRRv", ".vvvvvvvvvv",
};
static uint8_t book_color(char ch) {
  switch (ch) { case 'k': return UMBER; case 'R': return OXBLOOD; case 'g': return OCHRE; case 'h': return LAMP; default: return VELLUM; }
}
static void draw_small_book(uint8_t *fb, float x, float y, bool warm, float t) {
  float pulse = 0.5f + 0.5f * sinf(t * 2.1f);
  glow(fb, x, y, 15, warm ? UMBER : SHADE, 0.6f);
  glow(fb, x, y, 11, warm ? EMBER : DUSK, 0.32f + 0.18f * pulse);
  glow(fb, x, y, 7, LAMP, (warm ? 0.3f : 0.16f) + 0.14f * pulse);
  int ox = iround(x - 5), oy = iround(y - 4);
  for (int r = 0; r < 8; r++)
    for (int c = 0; SMALL_BOOK[r][c]; c++) {
      char ch = SMALL_BOOK[r][c];
      if (ch != '.') pset(fb, ox + c, oy + r, book_color(ch));
    }
}

// Folder-name tab: a 2-character plate riding the railing, with a folder notch.
enum { TAB_KNOWN, TAB_FLASH, TAB_FLICKER, TAB_FLICKER2 };
static void draw_tab(uint8_t *fb, float cx, float cy, const char *text, int style, float a) {
  int x0 = iround(cx - 7), y0 = iround(cy - 5);
  uint8_t fill = style == TAB_FLASH ? WHITE : INK;
  uint8_t edge = style == TAB_KNOWN ? CYAN : style == TAB_FLASH ? WHITE : DUSK;
  for (int i = 1; i <= 5; i++) dset(fb, x0 + i, y0 - 1, edge, a);          // folder notch
  for (int j = 0; j < 11; j++) for (int i = 0; i < 15; i++) {
    bool border = i == 0 || j == 0 || i == 14 || j == 10;
    dset(fb, x0 + i, y0 + j, border ? edge : fill, a);
  }
  int tx = x0 + 2;
  for (const char *s = text; *s; s++) {
    uint8_t shown = *s == ' ' ? 0x82 : (uint8_t)*s;
    uint8_t col = style == TAB_FLASH ? INK : *s == ' ' ? DUSK : style == TAB_KNOWN ? CYAN : style == TAB_FLICKER2 ? DUSK : VELLUM;
    raw_char5(fb, shown, tx, y0 + 2, col, a);
    tx += 6;
  }
}

/* ============================================================= scene bits */
static const uint8_t *content(bs_scene_t *sc, int *len) {
  if (sc->content.v != sc->path_version || sc->content.n != sc->reveal) {
    sc->content.v = sc->path_version;
    sc->content.n = sc->reveal;
    sc->content.len = bs_content_of(sc->mode, sc->path, sc->reveal, sc->content.bytes, BS_CONTENT_MAX);
  }
  *len = sc->content.len;
  return sc->content.bytes;
}

/* ============================================================= the book */
#define PAGE_W 70
#define PAGE_H 84
static uint8_t cover_pixel(float fx, float fy, int x, int y, float w, float h) {
  // gilt frame inset 4px, hexagon emblem
  float W = fabsf(w), px = fx * W, py = fy * h;
  if (px < 2) return UMBER;                                    // spine shadow
  if (W > 20 && ((fabsf(px - 5) < 0.5f || fabsf(px - (W - 4)) < 0.5f) && py > 4 && py < h - 4)) return OCHRE;
  if (W > 20 && ((fabsf(py - 4.5f) < 0.5f || fabsf(py - (h - 4.5f)) < 0.5f) && px > 4.5f && px < W - 3.5f)) return OCHRE;
  // emblem: hexagon ring centred at 42% height
  if (W > 30) {
    float ex = px - W * 0.53f, ey = py - h * 0.40f;
    float ap = fmaxf(fabsf(ey), 0.866f * fabsf(ex) + 0.5f * fabsf(ey));
    if (ap > 7.2f && ap < 8.6f) return OCHRE;
    if (ap < 3) return LAMP;
  }
  return bayer(x, y) < 0.18f ? UMBER : OXBLOOD;
}
// The cover: a trapezoid hinging on the spine, left edge (x0, height h0), free edge (x1, h1).
static void cover_quad(uint8_t *fb, float x0, float y0, float h0, float x1, float h1, float w, float h) {
  float xa = fminf(x0, x1), xb = fmaxf(x0, x1), cy = y0 + h0 / 2;
  for (int x = (int)floorf(xa); x < (int)ceilf(xb); x++) {
    float f = xb - xa < 1 ? 0 : (x + 0.5f - x0) / (x1 - x0);
    float fc = clampf(f, 0, 1), hh = lerpf(h0, h1, fc), top = cy - hh / 2;
    for (int y = (int)floorf(top); y < (int)ceilf(top + hh); y++)
      pset(fb, x, y, cover_pixel(fc, (y + 0.5f - top) / hh, x, y, w, h));
  }
}
static void page(uint8_t *fb, float x, float y, float w, float h, bool right, bs_scene_t *sc, float open) {
  if (w < 1) return;
  fill_rect(fb, x, y, w, h, VELLUM, 1);
  // gutter shadow toward the spine
  for (int i = 0; i < 4; i++) {
    float gx = right ? x + i : x + w - 1 - i;
    fill_rect(fb, gx, y, 1, h, DUSK, 0.45f * (1 - i / 4.0f));
  }
  fill_rect(fb, x, y + h - 1, w, 1, OCHRE, 0.5f);           // deckle
  if (open < 1 || w < PAGE_W - 2) return;
  float fade = clampf((sc->t - sc->book.open_at - 0.86f) / 0.12f, 0, 1);
  if (fade <= 0) return;
  int len;
  const uint8_t *bytes = content(sc, &len);
  char buf[48];
  if (!right) {
    // verso: the raw bytes, laid out like a hex editor
    float tx = x + 6;
    text5(fb, "file", tx, y + 6, OXBLOOD, -1, fade);
    snprintf(buf, sizeof buf, "%d B", len);
    text3(fb, buf, x + w - 6 - w3(buf), y + 8, DUSK, fade);
    for (int i = (int)(x + 6); i < x + w - 6; i++) dset(fb, i, iround(y + 16), DUSK, 0.6f * fade);
    const int per = 3, max_lines = 7;
    int lines = (len + per - 1) / per;
    if (lines > max_lines) lines = max_lines;
    for (int l = 0; l < lines; l++) {
      int o = 0;
      for (int i = l * per; i < l * per + per && i < len; i++) {
        if (o) buf[o++] = ' ';
        buf[o++] = HEXD[bytes[i] >> 4]; buf[o++] = HEXD[bytes[i] & 15];
      }
      buf[o] = 0;
      if (l == max_lines - 1 && len > per * max_lines) { buf[5] = ' '; buf[6] = (char)0x81; buf[7] = 0; }
      char off[3] = { HEXD[((l * per) >> 4) & 15], HEXD[(l * per) & 15], 0 };
      text3(fb, off, tx, y + 22 + l * 9, DUSK, fade);
      text5(fb, buf, tx + 10, y + 21 + l * 9, INK, -1, fade);
    }
    if (!len) text5(fb, "(empty)", tx, y + 21, DUSK, -1, fade);
  } else {
    // recto: the same bytes as text, opened with an illuminated initial
    float tx = x + 9;
    int start = 0, line_x0 = (int)tx, line_n0 = 9;
    if (len && bytes[0] > 32 && bytes[0] < 127) {
      big_glyph(fb, bytes[0], iround(tx + 1), iround(y + 9), OCHRE, 2, fade);
      big_glyph(fb, bytes[0], iround(tx), iround(y + 8), OXBLOOD, 2, fade);
      start = 1; line_x0 = (int)tx + 13; line_n0 = 7;
    }
    int i = start;
    for (int l = 0; l < 7 && i < len; l++) {
      int n = l < 2 ? line_n0 : 9;
      int xx = l < 2 ? line_x0 : (int)tx;
      int end = i + n < len ? i + n : len;
      bool more = end < len;
      for (int j = i; j < end; j++) {
        uint8_t b = bytes[j];
        bool printable = b >= 32 && b < 127;
        uint8_t ch = (l == 6 && more && j == end - 1) ? 0x81 : printable ? b : 0x80;
        raw_char5(fb, ch, xx, iround(y + 9 + l * 9 + (l < 2 ? 3 : 0)), (ch == 0x80 || ch == 0x81) ? DUSK : INK, fade);
        xx += 6;
      }
      i = end;
    }
    if (!len) text5(fb, "(empty)", tx, y + 9, DUSK, -1, fade);
  }
}
static bool draw_open_book(uint8_t *fb, bs_scene_t *sc, const bs_params_t *P) {
  const float RISE = 0.5f, OPEN = 0.36f, SHUT = 0.3f, SINK = 0.42f;
  float t = sc->t, rise, open;
  float ta = t - sc->book.open_at;
  if (sc->book.close_at < 0 || t < sc->book.close_at) { rise = clampf(ta / RISE, 0, 1); open = clampf((ta - RISE) / OPEN, 0, 1); }
  else { float tc = t - sc->book.close_at; open = 1 - clampf(tc / SHUT, 0, 1); rise = 1 - clampf((tc - SHUT) / SINK, 0, 1); }
  if (rise <= 0) return false;
  float er = ease_out(rise);
  darken_rect(fb, 0, 0, SW, HUD_Y, 2.2f * er);

  // Closed volume grows from the speck at the vanishing point.
  const float cx = 120, cy = 56;
  float sx = lerpf(P->vpx - 5, cx, er), sy = lerpf(P->vpy - 4, cy - PAGE_H / 2.0f, er);
  float w = lerpf(11, PAGE_W, er), h = lerpf(8, PAGE_H, er);
  float theta = ease_in_out(open) * PI_F;
  float ct = cosf(theta), st = sinf(theta);

  if (open > 0) page(fb, sx, sy, w, h, true, sc, open);       // right-hand page, under the cover
  float lift = 7 * st;
  if (ct >= 0) {
    cover_quad(fb, sx, sy, h, sx + w * ct, h + lift, w * ct, h);
    if (open == 0) {                                          // page block edge beside the cover
      fill_rect(fb, sx + w, sy + 2, fmaxf(1, er * 3), h - 2, VELLUM, 1);
      fill_rect(fb, sx + 2, sy + h, w - 1, fmaxf(1, er * 2), DUSK, 1);
    }
  } else {
    page(fb, sx + w * ct, sy - lift / 2, -w * ct, h + lift, false, sc, open);
  }
  // the cover is stamped with the name of the folder the book lives in
  if (ct > 0.97f && er > 0.85f) {
    char title[3] = "/";
    if (sc->reveal) bs_name(sc->mode, sc->path[sc->reveal - 1], title);
    float ex = sx + w * 0.53f, ey = sy + h * 0.4f + 13;
    int tx = iround(ex - w5(title) / 2.0f);
    for (const char *s = title; *s; s++) { raw_char5(fb, *s == ' ' ? 0x82 : (uint8_t)*s, tx, iround(ey), OCHRE, clampf((er - 0.85f) / 0.15f, 0, 1)); tx += 6; }
  }
  if (er > 0.3f) fill_rect(fb, sx - 1, sy - 1, 2, h + 2, UMBER, 1);   // the spine
  return true;
}

/* ================================================================== HUD */
#define PATH_X 5
#define PATH_Y 114
#define BODY_Y 125
#define PATH_MAX_CHARS 31

// Path layout: "/48/65/6C" with a leading ellipsis when it overflows, cut at an
// element boundary so a name is never split. Port note: computed arithmetically
// (every element is "/" plus two characters) instead of building run arrays.
static int path_cut(int count, int max_chars) {
  int total = count ? 3 * count : 1;
  if (total <= max_chars) return 0;
  int need = total - (max_chars - 1);
  int i = (need + 2) / 3;
  return i < count ? 3 * i : need;
}
#define SLOT_NONE INT_MIN
static int slot_x(int i, int cut) {
  int s = 3 * i + 1;
  if (!cut) return PATH_X + s * 6;
  int off = cut - 1;
  return s - off >= 1 ? PATH_X + (s - off) * 6 : SLOT_NONE;
}
static void name_run_char(const char name[3], int j, uint8_t *ch, uint8_t *col) {
  if (name[j] == ' ') { *ch = 0x82; *col = DUSK; } else { *ch = (uint8_t)name[j]; *col = CYAN; }
}
// Draws the path made of `count` components; returns the x after the last char.
static int draw_path(uint8_t *fb, int mode, const uint16_t *comps, int count, int x, int y) {
  int cut = path_cut(count, PATH_MAX_CHARS);
  int total = count ? 3 * count : 1;
  if (cut) x = raw_char5(fb, 0x81, x, y, DUSK, 1);
  for (int j = cut; j < total; j++) {
    if (j % 3 == 0) { x = raw_char5(fb, '/', x, y, DUSK, 1); continue; }
    char name[3];
    bs_name(mode, comps[j / 3], name);
    uint8_t ch, col;
    name_run_char(name, j % 3 - 1, &ch, &col);
    x = raw_char5(fb, ch, x, y, col, 1);
  }
  return x;
}
// Name runs with a dark keyline, for the flyers.
static void draw_name_runs(uint8_t *fb, const char name[3], float fx, float fy, int outline) {
  int x = iround(fx), y = iround(fy);
  for (int j = 0; j < 2; j++) {
    uint8_t ch, col;
    name_run_char(name, j, &ch, &col);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) raw_char5(fb, ch, x + j * 6 + dx, y + dy, (uint8_t)outline, 1);
  }
  for (int j = 0; j < 2; j++) {
    uint8_t ch, col;
    name_run_char(name, j, &ch, &col);
    raw_char5(fb, ch, x + j * 6, y, col, 1);
  }
}

static void draw_hud(uint8_t *fb, bs_scene_t *sc) {
  fill_rect(fb, 0, HUD_Y, SW, SH - HUD_Y, INK, 1);
  fill_rect(fb, 0, HUD_Y, SW, 1, SHADE, 1);
  for (int x = 3; x < 197; x++) dset(fb, x, HUD_Y + 11, OXBLOOD, 0.5f);   // catalog-card rule

  // call number: the path
  int end_x;
  bool wiping = false;
  if (sc->wipe.active) {
    float p = clampf((sc->t - sc->wipe.t0) / sc->wipe.dur, 0, 1);
    if (p < 1) {
      wiping = true;
      int n = (int)ceilf(sc->wipe.n * (1 - ease_in(p)));
      end_x = draw_path(fb, sc->mode, sc->wipe.comps, n, PATH_X, PATH_Y);
    } else sc->wipe.active = false;
  }
  if (!wiping) end_x = draw_path(fb, sc->mode, sc->path, sc->reveal, PATH_X, PATH_Y);
  if (sc->host && !wiping && ((int)floorf(sc->t * 1.7f)) % 2 == 0) fill_rect(fb, end_x, PATH_Y + 6, 5, 1, CYAN, 1);

  // title: the file at the bottom of this folder, as text
  int len;
  const uint8_t *bytes = content(sc, &len);
  if (sc->toast.active && sc->t - sc->toast.t0 < sc->toast.dur) {
    int blink = sc->t - sc->toast.t0 < 0.5f ? ((int)floorf((sc->t - sc->toast.t0) * 12)) % 2 : 0;
    text5(fb, sc->toast.text, PATH_X, BODY_Y, blink ? VELLUM : sc->toast.color, -1, 1);
  } else {
    sc->toast.active = false;
    int x = PATH_X, from = 0;
    if (len > PATH_MAX_CHARS) { x = raw_char5(fb, 0x81, x, BODY_Y, DUSK, 1); from = len - (PATH_MAX_CHARS - 1); }
    if (!len && sc->host) raw_text5(fb, "empty file", x, BODY_Y, DUSK, 1);
    bool fresh = sc->t - sc->reveal_at < 0.35f;
    for (int i = from; i < len; i++) {
      uint8_t b = bytes[i];
      bool printable = b >= 32 && b < 127;
      uint8_t col = !printable ? DUSK : (i == len - 1 && fresh) ? WHITE : VELLUM;
      x = raw_char5(fb, printable ? b : 0x80, x, BODY_Y, col, 1);
    }
  }

  // depth and size, right-aligned
  char depth[8], size[12];
  snprintf(depth, sizeof depth, "%02d", sc->reveal);
  snprintf(size, sizeof size, "%d", len);
  text3(fb, "LV", 205, PATH_Y + 1, DUSK, 1);
  text5(fb, depth, 236 - w5(depth), PATH_Y, sc->host ? VELLUM : DUSK, -1, 1);
  text5(fb, size, 230 - w5(size), BODY_Y, sc->host ? VELLUM : DUSK, -1, 1);
  text3(fb, "B", 232, BODY_Y + 2, DUSK, 1);
}

// a gallery seen from above, with the shaft in its middle
static const char *const HEXICON[8] = { "..#####..", ".#.....#.", ".#..o..#.", "#..ooo..#", "#..ooo..#", ".#..o..#.", ".#.....#.", "..#####.." };

static void draw_top_bar(uint8_t *fb, bs_scene_t *sc) {
  const int hx = 3, hy = 2;
  for (int r = 0; r < 8; r++) for (int c = 0; c < 9; c++)
    if (HEXICON[r][c] != '.') for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) pset(fb, hx + c + dx, hy - 1 + r + dy, VOID);
  for (int r = 0; r < 8; r++) for (int c = 0; c < 9; c++) {
    char ch = HEXICON[r][c];
    if (ch == '#') pset(fb, hx + c, hy - 1 + r, OCHRE);
    else if (ch == 'o') pset(fb, hx + c, hy - 1 + r, sc->host ? LAMP : DUSK);
  }
  int wx = text5(fb, "USB OF BABEL", 15, 2, OCHRE, VOID, 1);
  // firmware: which naming the drive is serving
  const char *mode = sc->mode == BS_MODE_HEX ? "HEX" : "B70";
  fill_rect(fb, wx + 2, 3, w3(mode) + 2, 7, VOID, 1);
  text3(fb, mode, wx + 3, 4, DUSK, 1);

  // status: session + activity LED
  uint8_t led = sc->t - sc->led_at < 0.09f ? CRIMSON : sc->host ? CYAN : DUSK;
  const char *label = sc->host == BS_HOST_MTP ? "MTP" : sc->host == BS_HOST_LOOKUP ? "LOOKUP" : "NO HOST";
  int lx = 236 - w5(label);
  text5(fb, label, lx, 2, sc->host ? VELLUM : DUSK, VOID, 1);
  fill_rect(fb, lx - 7, 3, 5, 5, VOID, 1);
  fill_rect(fb, lx - 6, 4, 3, 3, led, 1);

  // firmware: a battery that only shows up when it matters
  if (sc->battery_pct >= 0 && sc->battery_pct < 20) {
    int bx = lx - 22, by = 3;
    bool blink = sc->battery_pct < 8 && ((int)floorf(sc->t * 2)) % 2;
    fill_rect(fb, bx - 1, by - 1, 14, 7, VOID, 1);
    uint8_t c = blink ? DUSK : CRIMSON;
    for (int i = 0; i < 11; i++) { pset(fb, bx + i, by, c); pset(fb, bx + i, by + 4, c); }
    for (int j = 0; j < 5; j++) { pset(fb, bx, by + j, c); pset(fb, bx + 10, by + j, c); }
    pset(fb, bx + 11, by + 1, c); pset(fb, bx + 11, by + 2, c); pset(fb, bx + 11, by + 3, c);
    int fillw = sc->battery_pct * 9 / 20;
    if (fillw < 1) fillw = 1;
    fill_rect(fb, bx + 1, by + 1, fillw, 3, c, 1);
  }
  if (sc->show_fps) {
    char buf[24];
    if (sc->free_kb >= 0) snprintf(buf, sizeof buf, "%d FPS %dK", (int)(sc->fps + 0.5f), sc->free_kb);
    else snprintf(buf, sizeof buf, "%d FPS", (int)(sc->fps + 0.5f));
    fill_rect(fb, 236 - w3(buf) - 1, 11, w3(buf) + 2, 7, VOID, 1);
    text3(fb, buf, 236 - w3(buf), 12, DUSK, 1);
  }
}

static void draw_flyers(uint8_t *fb, bs_scene_t *sc) {
  for (int i = 0; i < sc->n_flyers; i++) {
    const bs_flyer_t *f = &sc->flyers[i];
    float p = clampf((sc->t - f->t0) / f->dur, 0, 1), e = ease_in_out(p);
    float x = lerpf(f->x0, f->x1, e), y = lerpf(f->y0, f->y1, e) - sinf(p * PI_F) * f->arc;
    char name[3];
    bs_name(sc->mode, f->comp, name);
    if (f->kind == 0) {
      if (p < 0.45f) draw_tab(fb, x + 7 - 2, y + 5 - 2, name, TAB_KNOWN, 1);
      else draw_name_runs(fb, name, x, y, VOID);
    } else {
      draw_name_runs(fb, name, x, y - p * 10, VOID);
    }
  }
}

// Boot splash (firmware polish): the gallery icon, the wordmark, a Borges line.
static void draw_splash(uint8_t *fb, bs_scene_t *sc) {
  float t = sc->t - sc->splash_t0;
  const float HOLD = 3.4f, OUT = 0.7f;
  if (sc->splash_t0 < 0 || t >= HOLD + OUT) { sc->splash_t0 = -1; return; }
  float out = t > HOLD ? (t - HOLD) / OUT : 0;
  darken_rect(fb, 0, 0, SW, SH, 3.2f * (1 - out));
  float a_icon = clampf(t / 0.6f, 0, 1) * (1 - out);
  float a_word = clampf((t - 0.5f) / 0.6f, 0, 1) * (1 - out);
  float a_text = clampf((t - 1.2f) / 0.8f, 0, 1) * (1 - out);
  const int s = 3, ix = 120 - 9 * s / 2, iy = 14;
  for (int r = 0; r < 8; r++) for (int c = 0; c < 9; c++) {
    char ch = HEXICON[r][c];
    if (ch == '.') continue;
    uint8_t col = ch == '#' ? OCHRE : LAMP;
    for (int dy = 0; dy < s; dy++) for (int dx = 0; dx < s; dx++) dset(fb, ix + c * s + dx, iy + r * s + dy, col, a_icon);
  }
  const char *word = "USB OF BABEL";
  int wx = 120 - ((int)strlen(word) * 12 - 2) / 2;
  for (const char *p = word; *p; p++) { big_glyph(fb, (uint8_t)*p, wx, 46, OCHRE, 2, a_word); wx += 12; }
  static const char *const QUOTE[4] = {
    "The universe (which others call the",
    "Library) is composed of an indefinite,",
    "perhaps infinite number of hexagonal",
    "galleries.",
  };
  for (int i = 0; i < 4; i++) raw_text5(fb, QUOTE[i], 120 - w5(QUOTE[i]) / 2.0f, 72 + i * 10, VELLUM, a_text);
  raw_text5(fb, "J. L. BORGES", 236 - w5("J. L. BORGES"), 114, DUSK, a_text);
}

/* ================================================================ scene */
enum { A_WAIT, A_CONNECT, A_DISCONNECT, A_ENTER, A_UP, A_ROOT, A_OPEN, A_CLOSE };

static void particle_init(bs_scene_t *sc, bs_particle_t *p, bool initial) {
  uint32_t *r = &sc->rnd;
  p->phi = rnd_f(r) * PI_F * 2; p->rho = 0.22f + rnd_f(r) * 0.62f;
  p->v = sc->cam_z + (initial ? 1.5f + rnd_f(r) * 13 : 10 + rnd_f(r) * 5);
  p->speed = 0.18f + rnd_f(r) * 0.22f; p->g = (int)(rnd_f(r) * 16); p->flip = 1 + rnd_f(r) * 3; p->a = 0.35f + rnd_f(r) * 0.45f;
}

static inline bool landed_get(const bs_scene_t *sc, int i) { return i >= 0 && i <= BS_PATH_MAX && (sc->landed[i >> 3] >> (i & 7)) & 1; }
static inline void landed_set(bs_scene_t *sc, int i) { if (i >= 0 && i <= BS_PATH_MAX) sc->landed[i >> 3] |= (uint8_t)(1 << (i & 7)); }
static inline void landed_del(bs_scene_t *sc, int i) { if (i >= 0 && i <= BS_PATH_MAX) sc->landed[i >> 3] &= (uint8_t)~(1 << (i & 7)); }
static inline void landed_clear(bs_scene_t *sc) { memset(sc->landed, 0, sizeof sc->landed); }
// Every landed level is relative to level_base; shifting the base shifts the set.
static void landed_shift(bs_scene_t *sc, int drop) {
  uint8_t old[sizeof sc->landed];
  memcpy(old, sc->landed, sizeof old);
  landed_clear(sc);
  for (int i = drop; i <= BS_PATH_MAX; i++) if ((old[i >> 3] >> (i & 7)) & 1) landed_set(sc, i - drop);
}

void bs_scene_init(bs_scene_t *sc, int mode, uint32_t seed) {
  memset(sc, 0, sizeof *sc);
  sc->mode = mode;
  sc->rnd = rng_seed((seed ? seed : 1) * 7919u + 17u);
  sc->reveal_at = -9;
  sc->vel = FALL_SPEED;
  sc->lamp.t0 = -99;
  sc->lantern.on = true; sc->lantern.t0 = -99;
  sc->led_at = -9;
  sc->book.open_at = -99; sc->book.close_at = -99;
  sc->auto_tilt = true;
  sc->bob_freq = 1.35f;
  sc->content.v = UINT32_MAX;
  sc->splash_t0 = -1;
  sc->battery_pct = -1;
  sc->free_kb = -1;
  for (int i = 0; i < BS_PARTICLES; i++) particle_init(sc, &sc->particles[i], true);
}

static float rest(const bs_scene_t *sc, int depth) { return sc->level_base + depth + 1 - Z_NEXT; }
static void emit(bs_scene_t *sc) { sc->led_at = sc->t; }

void bs_toast(bs_scene_t *sc, const char *text, uint8_t color, float dur) {
  sc->toast.active = true;
  strncpy(sc->toast.text, text, sizeof sc->toast.text - 1);
  sc->toast.text[sizeof sc->toast.text - 1] = 0;
  sc->toast.color = color;
  sc->toast.t0 = sc->t;
  sc->toast.dur = dur;
}

static void start_wipe(bs_scene_t *sc, float dur) {
  int n = sc->reveal < sc->path_len ? sc->reveal : sc->path_len;
  memcpy(sc->wipe.comps, sc->path, n * sizeof(uint16_t));
  sc->wipe.n = n; sc->wipe.t0 = sc->t; sc->wipe.dur = dur; sc->wipe.active = true;
}
static void reset_path(bs_scene_t *sc) {
  sc->path_len = 0; sc->reveal = 0; landed_clear(sc); sc->path_version++; sc->epoch++;
}

void bs_set_mode(bs_scene_t *sc, int mode) {
  if (mode == sc->mode) return;
  sc->mode = mode;
  reset_path(sc);
  sc->q_len = 0; sc->act.active = false; sc->n_flyers = 0;
  if (sc->host) { sc->level_base = (int)ceilf(sc->cam_z - 1 + Z_NEXT); sc->cam_z = rest(sc, 0); }
}

/* ---------------------------------------------------------- actions */
static bs_action_t *push(bs_scene_t *sc, int kind, float dur) {
  if (sc->q_len >= BS_QUEUE_MAX) return NULL;
  bs_action_t *a = &sc->queue[sc->q_len++];
  memset(a, 0, sizeof *a);
  a->kind = (uint8_t)kind; a->dur = dur;
  return a;
}
int bs_queue_pending(const bs_scene_t *sc) { return sc->q_len + (sc->act.active ? 1 : 0); }
void bs_wait(bs_scene_t *sc, float d) { push(sc, A_WAIT, d); }
void bs_connect(bs_scene_t *sc, int host_kind) { bs_action_t *a = push(sc, A_CONNECT, 1); if (a) a->comp = (uint16_t)host_kind; }
void bs_disconnect(bs_scene_t *sc) { push(sc, A_DISCONNECT, 1.2f); }
static float enter_lock(int fast) { return fast == 2 ? 0.03f : fast ? 0.06f : 0.16f; }
static float enter_move(int fast) { return fast == 2 ? 0.12f : fast ? 0.26f : 0.46f; }
void bs_enter(bs_scene_t *sc, uint16_t comp, int fast) {
  bs_action_t *a = push(sc, A_ENTER, enter_lock(fast) + enter_move(fast));
  if (a) { a->comp = comp; a->fast = (uint8_t)fast; }
}
void bs_up(bs_scene_t *sc, int fast) {
  bs_action_t *a = push(sc, A_UP, fast ? 0.22f : 0.44f);
  if (a) a->fast = (uint8_t)fast;
}
void bs_root(bs_scene_t *sc) { push(sc, A_ROOT, 0.6f); }
void bs_open(bs_scene_t *sc) { push(sc, A_OPEN, 0.95f); }
void bs_close(bs_scene_t *sc) { push(sc, A_CLOSE, 0.75f); }
static void close_book(bs_scene_t *sc) { sc->book.close_at = sc->t; sc->book.is_open = false; }

void bs_navigate_to(bs_scene_t *sc, const uint16_t *target, int n, float interval, bool replace) {
  // plan from the path as it will be once the queue drains; a live lookup
  // (replace) drops whatever was still pending and re-plans from here
  if (n > BS_PATH_MAX) n = BS_PATH_MAX;
  if (replace) {
    int k = 0;
    for (int i = 0; i < sc->q_len; i++) if (sc->queue[i].kind == A_CONNECT) sc->queue[k++] = sc->queue[i];
    sc->q_len = k;
  }
  static uint16_t cur[BS_PATH_MAX];
  int cn = sc->path_len;
  memcpy(cur, sc->path, cn * sizeof(uint16_t));
  int host = sc->host;
  for (int i = 0; i < sc->q_len; i++) {
    const bs_action_t *a = &sc->queue[i];
    if (a->kind == A_ENTER) { if (cn < BS_PATH_MAX) cur[cn++] = a->comp; }
    else if (a->kind == A_UP) { if (cn) cn--; }
    else if (a->kind == A_ROOT) cn = 0;
    else if (a->kind == A_CONNECT && host != BS_HOST_MTP && host != a->comp) { cn = 0; host = a->comp; }  // port note: a connect resets the path
  }
  int common = 0;
  while (common < cn && common < n && cur[common] == target[common]) common++;
  int ups = cn - common, downs = n - common;
  int fast = ups + downs > 4 ? (downs > 24 ? 2 : 1) : 0;
  if (ups > 2 && common == 0) bs_root(sc);
  else for (int i = 0; i < ups; i++) bs_up(sc, 1);
  for (int i = common; i < n; i++) {
    bs_enter(sc, target[i], interval > 0 ? 0 : fast);
    if (interval > 0 && i < n - 1) bs_wait(sc, interval);
  }
}

void bs_goto_bytes(bs_scene_t *sc, const uint8_t *bytes, int len, float interval, bool replace) {
  static uint16_t target[BS_PATH_MAX];
  int n = bs_path_for(sc->mode, bytes, len, target, BS_PATH_MAX);
  bs_navigate_to(sc, target, n, interval, replace);
}

void bs_toggle_book(bs_scene_t *sc) {
  if (sc->book.is_open) { bs_close(sc); return; }
  if (!sc->host) bs_connect(sc, BS_HOST_LOOKUP);
  bs_open(sc);
}

// Returns the action's duration: < 0 keeps the queued duration, 0 makes it instant.
static float action_start(bs_scene_t *sc, bs_action_t *a) {
  switch (a->kind) {
    case A_CONNECT: {
      if (sc->host == BS_HOST_MTP || (sc->host && sc->host == a->comp)) return 0;
      bool lit = sc->host != BS_HOST_NONE;
      sc->host = a->comp;
      if (!lit) {
        sc->lamp.on = true; sc->lamp.t0 = sc->t + 0.3f; sc->lamp.origin = (int)floorf(sc->cam_z); sc->lamp.from = 0;
        sc->lantern.on = false; sc->lantern.t0 = sc->t + 0.2f;
      }
      start_wipe(sc, 0.45f);
      reset_path(sc);
      float z0 = sc->cam_z, v = fmaxf(sc->vel, 0.05f);
      sc->level_base = (int)ceilf(z0 - 1 + Z_NEXT + fmaxf(0.3f, v * 0.7f));
      float dist = rest(sc, 0) - z0;
      sc->mv.z0 = z0; sc->mv.dist = dist;
      float dur = clampf(2 * dist / v, 0.5f, 1.5f);
      sc->vel = 0;
      if (a->comp == BS_HOST_MTP) bs_toast(sc, "HOST CONNECTED", LAMP, 1.6f);
      else bs_toast(sc, "CATALOG LOOKUP", LAMP, 1.6f);
      emit(sc);
      return dur;
    }
    case A_DISCONNECT:
      if (!sc->host) return 0;
      if (sc->book.is_open) close_book(sc);
      bool was_lookup = sc->host == BS_HOST_LOOKUP;
      sc->host = BS_HOST_NONE;
      sc->lamp.on = false; sc->lamp.t0 = sc->t; sc->lamp.origin = (int)floorf(sc->cam_z); sc->lamp.from = 1;
      sc->lantern.on = true; sc->lantern.t0 = sc->t + 0.4f;
      bs_toast(sc, was_lookup ? "BACK TO FREEFALL" : "HOST GONE " BS_CH_MIDDOT " FREEFALL", DUSK, 1.6f);
      emit(sc);
      return -1;
    case A_ENTER: {
      if (sc->book.is_open) close_book(sc);
      if (sc->path_len >= BS_PATH_MAX) return 0;
      sc->path[sc->path_len++] = a->comp; sc->path_version++;
      sc->lock_fx.active = true; sc->lock_fx.L = sc->level_base + sc->path_len; sc->lock_fx.t0 = sc->t;
      float lock = enter_lock(a->fast), move = enter_move(a->fast);
      sc->mv.z0 = sc->cam_z; sc->mv.z1 = rest(sc, sc->path_len); sc->mv.lock = lock / (lock + move);
      emit(sc);
      return -1;
    }
    case A_UP: {
      if (!sc->path_len) return 0;
      if (sc->book.is_open) close_book(sc);
      int L = sc->level_base + sc->path_len;
      uint16_t comp = sc->path[--sc->path_len]; sc->path_version++; sc->epoch++;
      int shown = sc->reveal < sc->path_len ? sc->reveal : sc->path_len;
      int sx = slot_x(shown, path_cut(shown + 1, PATH_MAX_CHARS));
      if (sx != SLOT_NONE && sc->reveal > sc->path_len && sc->n_flyers < BS_FLYERS_MAX) {
        bs_flyer_t *f = &sc->flyers[sc->n_flyers++];
        memset(f, 0, sizeof *f);
        f->kind = 1; f->comp = comp; f->x0 = f->x1 = (float)sx; f->y0 = PATH_Y; f->y1 = PATH_Y - 6; f->t0 = sc->t; f->dur = 0.3f; f->arc = 0;
      }
      if (sc->reveal > sc->path_len) sc->reveal = sc->path_len;
      landed_del(sc, L - sc->level_base);
      float move = a->dur;
      sc->unlock_fx.active = true; sc->unlock_fx.L = L; sc->unlock_fx.comp = comp; sc->unlock_fx.t0 = sc->t + move;
      sc->mv.z0 = sc->cam_z; sc->mv.z1 = rest(sc, sc->path_len);
      emit(sc);
      return -1;
    }
    case A_ROOT: {
      if (sc->book.is_open) close_book(sc);
      int n = sc->path_len;
      float dur = clampf(0.4f + 0.07f * n, 0.45f, 1.3f);
      start_wipe(sc, dur * 0.8f);
      reset_path(sc);
      sc->mv.z0 = sc->cam_z; sc->mv.z1 = rest(sc, 0);
      emit(sc);
      return dur;
    }
    case A_OPEN:
      sc->book.open_at = sc->t; sc->book.close_at = -1; sc->book.is_open = true;
      emit(sc);
      return -1;
    case A_CLOSE:
      if (sc->book.is_open) close_book(sc);
      else return 0;
      return -1;
    default:
      return -1;
  }
}
static void action_update(bs_scene_t *sc, const bs_action_t *a, float p) {
  switch (a->kind) {
    case A_CONNECT: sc->cam_z = sc->mv.z0 + sc->mv.dist * (1 - (1 - p) * (1 - p)); break;
    case A_DISCONNECT: sc->vel = FALL_SPEED * ease_in(p); break;
    case A_ENTER: {
      float q = clampf((p - sc->mv.lock) / (1 - sc->mv.lock), 0, 1);
      sc->cam_z = lerpf(sc->mv.z0, sc->mv.z1, ease_in_out(q));
      sc->fov_kick = sinf(q * PI_F);
      break;
    }
    case A_UP: sc->cam_z = lerpf(sc->mv.z0, sc->mv.z1, ease_in_out(p)); sc->fov_kick = -0.6f * sinf(p * PI_F); break;
    case A_ROOT: sc->cam_z = lerpf(sc->mv.z0, sc->mv.z1, ease_in_out(p)); sc->fov_kick = -sinf(p * PI_F); break;
    default: break;
  }
}
static void action_end(bs_scene_t *sc, const bs_action_t *a) {
  switch (a->kind) {
    case A_DISCONNECT: sc->vel = FALL_SPEED; break;
    case A_ENTER: case A_UP: case A_ROOT: sc->cam_z = sc->mv.z1; sc->fov_kick = 0; break;
    default: break;
  }
}

/* ------------------------------------------------------------ update */
void bs_update(bs_scene_t *sc, float dt) {
  sc->t += dt;

  // run queued actions
  if (!sc->act.active && sc->q_len) {
    bs_action_t a = sc->queue[0];
    memmove(sc->queue, sc->queue + 1, (sc->q_len - 1) * sizeof(bs_action_t));
    sc->q_len--;
    float d = action_start(sc, &a);
    sc->act.active = true; sc->act.a = a; sc->act.t0 = sc->t;
    sc->act.dur = d == 0 ? 0 : d > 0 ? d : a.dur;
  }
  if (sc->act.active) {
    float dur = sc->act.dur;
    float p = dur > 0 ? clampf((sc->t - sc->act.t0) / dur, 0, 1) : 1;
    if (dur > 0) action_update(sc, &sc->act.a, p);
    if (p >= 1) { if (dur > 0) action_end(sc, &sc->act.a); sc->act.active = false; }
  }

  // freefall while nobody is browsing: the librarian wanders
  int k = sc->act.active ? sc->act.a.kind : -1;
  bool moving = k == A_CONNECT || k == A_ENTER || k == A_UP || k == A_ROOT;
  if (!sc->host && !moving) {
    sc->cam_z += sc->vel * dt;
    int next_l = sc->level_base + sc->path_len + 1;
    if (next_l + TAB_FV - sc->cam_z < Z_LOCK && sc->path_len < BS_PATH_MAX) {
      sc->path[sc->path_len++] = (uint16_t)(rnd_f(&sc->rnd) * bs_fanout(sc->mode)); sc->path_version++;
      sc->lock_fx.active = true; sc->lock_fx.L = next_l; sc->lock_fx.t0 = sc->t;
    }
    if (sc->path_len > 48) {
      const int drop = 24;
      memmove(sc->path, sc->path + drop, (sc->path_len - drop) * sizeof(uint16_t));
      sc->path_len -= drop; sc->level_base += drop;
      landed_shift(sc, drop);
      sc->reveal = sc->reveal - drop > 0 ? sc->reveal - drop : 0; sc->path_version++; sc->epoch++;
      int j = 0;
      for (int i = 0; i < sc->n_flyers; i++) if (sc->flyers[i].kind != 0) sc->flyers[j++] = sc->flyers[i];
      sc->n_flyers = j;
    }
  }

  // a tab reaching the catalog card turns into a flyer bound for its slot
  bs_params_t P;
  bool have_p = false;
  for (int i = sc->reveal + 1; i <= sc->path_len; i++) {
    if (landed_get(sc, i)) continue;
    int L = sc->level_base + i;
    float z = L + TAB_FV - sc->cam_z;
    if (z >= Z_LAND) continue;
    landed_set(sc, i);
    if (sc->n_flyers >= BS_FLYERS_MAX) {   // port note: no room for a flyer, land at once
      if (sc->path_len >= i) { sc->reveal = sc->reveal > i ? sc->reveal : i; sc->reveal_at = sc->t; }
      continue;
    }
    if (!have_p) { bs_frame_params(sc, &P); have_p = true; }
    float ap[2];
    shaft_point(&P, 1.5f, fmaxf(z, 0.3f), ap);
    int sx = slot_x(i - 1, path_cut(i, PATH_MAX_CHARS));
    bs_flyer_t *f = &sc->flyers[sc->n_flyers++];
    memset(f, 0, sizeof *f);
    f->kind = 0; f->comp = sc->path[i - 1];
    f->x0 = ap[0] - 5; f->y0 = fminf(ap[1] - 3, HUD_Y - 8);
    f->x1 = sx != SLOT_NONE ? (float)sx : PATH_X + 6; f->y1 = PATH_Y;
    f->t0 = sc->t; f->dur = 0.3f; f->arc = 4; f->epoch = sc->epoch; f->idx = i;
  }
  int j = 0;
  for (int i = 0; i < sc->n_flyers; i++) {
    bs_flyer_t *f = &sc->flyers[i];
    if (sc->t - f->t0 >= f->dur) {
      if (f->kind == 0 && sc->epoch == f->epoch && sc->path_len >= f->idx) {
        if (f->idx > sc->reveal) sc->reveal = f->idx;
        sc->reveal_at = sc->t;
      }
      continue;
    }
    sc->flyers[j++] = *f;
  }
  sc->n_flyers = j;

  // data motes drift up out of the dark
  for (int i = 0; i < BS_PARTICLES; i++) {
    bs_particle_t *p = &sc->particles[i];
    p->v -= p->speed * dt;
    float z = p->v - sc->cam_z;
    if (z < 1.2f || z > 16) particle_init(sc, p, false);
  }

  // hand tremor / IMU tilt
  if (sc->auto_tilt) {
    sc->tilt.x = 5 * sinf(sc->t * 0.31f) + 2 * sinf(sc->t * 0.83f + 1);
    sc->tilt.y = 2.5f * sinf(sc->t * 0.23f + 1.3f);
    sc->tilt.roll = 0.035f * sinf(sc->t * 0.17f + 0.4f);
  }
  float kk = 1 - expf(-dt * 5);
  sc->tilt_s.x += (sc->tilt.x - sc->tilt_s.x) * kk;
  sc->tilt_s.y += (sc->tilt.y - sc->tilt_s.y) * kk;
  sc->tilt_s.roll += (sc->tilt.roll - sc->tilt_s.roll) * kk;
}

static float lamp_i(const bs_scene_t *sc, int L) {
  float delay = (L - sc->lamp.origin > 0 ? L - sc->lamp.origin : 0) * 0.075f;
  float p = clampf((sc->t - sc->lamp.t0 - delay) / 0.3f, 0, 1);
  float I = sc->lamp.on ? fminf(1.25f, sinf(p * PI_F / 2) + 0.45f * sinf(p * PI_F)) : sc->lamp.from * (1 - p);
  // "insufficient, incessant": a faint, constant flicker, and once in a while a stutter
  uint32_t n = hash2((int)floorf(sc->t * 9), L);
  I *= 0.93f + 0.07f * ((n & 255) / 255.0f);
  if (n % 211 == 0) I *= 0.55f;
  return I;
}
static float lantern_i(const bs_scene_t *sc) {
  float p = clampf((sc->t - sc->lantern.t0) / 0.9f, 0, 1);
  return sc->lantern.on ? ease_out(p) : 1 - p;
}

void bs_frame_params(const bs_scene_t *sc, bs_params_t *P) {
  float bob = sc->host ? 0.035f * sinf(sc->t * sc->bob_freq) : 0;
  P->vpx = VP_X + sc->tilt_s.x; P->vpy = VP_Y + sc->tilt_s.y; P->roll = sc->tilt_s.roll;
  P->cam_z = sc->cam_z + bob; P->K = K_BASE * (1 + 0.07f * sc->fov_kick);
  P->ambient = sc->host ? 0.52f : 0.4f;
  P->lantern = lantern_i(sc);
  P->sc = sc;
}

static bool tab_info(bs_scene_t *sc, int L, char name[3], int *style) {
  int i = L - sc->level_base;
  if (i < 1) return false;
  if (sc->unlock_fx.active && sc->unlock_fx.L == L && sc->t - sc->unlock_fx.t0 < 0.5f && i > sc->path_len) {
    bs_name(sc->mode, sc->unlock_fx.comp, name); *style = TAB_KNOWN; return true;
  }
  if (i <= sc->path_len) {
    bool flash = sc->lock_fx.active && sc->lock_fx.L == L && sc->t - sc->lock_fx.t0 < 0.1f;
    bs_name(sc->mode, sc->path[i - 1], name); *style = flash ? TAB_FLASH : TAB_KNOWN; return true;
  }
  int tick = (int)floorf(sc->t * 16);
  uint32_t s = rng_seed(hash2(tick, L) + 1);
  bs_name(sc->mode, (uint16_t)(rnd_f(&s) * bs_fanout(sc->mode)), name);
  *style = i == sc->path_len + 1 ? TAB_FLICKER : TAB_FLICKER2;
  return true;
}

/* ------------------------------------------------------------ render */
void bs_render_overlays(bs_scene_t *sc, const bs_params_t *P, uint8_t *fb) {
  draw_far_rings(fb, P);
  draw_lamps(fb, P);
  draw_particles(fb, P, sc->particles, sc->t);

  bool book_up = sc->book.open_at >= 0 && (sc->book.is_open || sc->t - sc->book.close_at < 0.75f);
  if (!book_up) {
    float bob = sinf(sc->t * 1.7f) * 1.2f;
    draw_small_book(fb, P->vpx, P->vpy + bob, sc->host != BS_HOST_NONE, sc->t);
  }

  // folder tabs on the bottom railing of the next floors, nearest first;
  // a tab that would collide with the one below it waits its turn
  float last_top = 1e9f;
  for (int L = (int)floorf(P->cam_z) + 1; L < P->cam_z + Z_TAB_MAX + 1; L++) {
    float z = L + TAB_FV - P->cam_z;
    int i = L - sc->level_base;
    if (z < Z_LAND || z > Z_TAB_MAX || landed_get(sc, i)) continue;
    char name[3];
    int style;
    if (!tab_info(sc, L, name, &style)) continue;
    float pt[2];
    shaft_point(P, 1.5f, z, pt);
    float ty = fminf(pt[1] - 1, HUD_Y - 7);
    if (ty + 8 > last_top) continue;
    float a = clampf((Z_TAB_MAX - z) / 0.4f, 0, 1) * (style == TAB_FLICKER2 ? 0.55f : 1);
    draw_tab(fb, pt[0], ty, name, style, a);
    last_top = ty - 7;
  }

  if (book_up) draw_open_book(fb, sc, P);
  draw_hud(fb, sc);
  draw_flyers(fb, sc);
  draw_top_bar(fb, sc);
  if (sc->splash_t0 >= 0) draw_splash(fb, sc);
}

void bs_render(bs_scene_t *sc, uint8_t *fb) {
  bs_params_t P;
  bs_frame_params(sc, &P);
  bs_render_shaft_rows(&P, fb, 0, SH);
  bs_render_overlays(sc, &P, fb);
}
