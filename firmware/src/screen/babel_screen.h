/*
 * babel_screen.h  ·  "The Air Shaft", ported from design/babel-screen.js
 *
 * Platform-independent: plain C99 + libm, single-precision float only. The
 * same file builds for the ESP32-S3 and for the host test in firmware/test/.
 *
 * Frame buffer: BS_W x BS_H bytes, one palette index (0..15) per pixel. The
 * display driver maps indices through BS_PALETTE565 (bs_tables.h).
 *
 * Folder components are 0-based folder indices in both naming modes:
 *   HEX  0..255   (handle = index + 1 + depth * 257)
 *   B70  0..4899  (handle = index + 1 + depth * 4901)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BS_W 240
#define BS_H 135
#define BS_PATH_MAX 256      // deepest path the screen tracks
#define BS_QUEUE_MAX 320     // queued scene actions
#define BS_FLYERS_MAX 12
#define BS_CONTENT_MAX 512   // bytes of `file` contents the screen shows
#define BS_PARTICLES 14

enum { BS_MODE_HEX = 0, BS_MODE_B70 = 1 };
enum { BS_HOST_NONE = 0, BS_HOST_MTP = 1, BS_HOST_LOOKUP = 2 };

// Palette indices (see BS_PALETTE565).
enum {
  BS_VOID, BS_INK, BS_SHADE, BS_DUSK, BS_UMBER, BS_WOOD, BS_OXBLOOD, BS_MOSS,
  BS_INDIGO, BS_OCHRE, BS_VELLUM, BS_LAMP, BS_EMBER, BS_CYAN, BS_WHITE, BS_CRIMSON
};

typedef struct {
  uint8_t kind;
  uint8_t fast;        // 0 normal, 1 fast, 2 very fast (long lookups)
  uint16_t comp;       // folder index for enter, host kind for connect
  float dur;
} bs_action_t;

typedef struct {
  uint8_t kind;        // 0 land, 1 lift
  uint16_t comp;
  float x0, y0, x1, y1, t0, dur, arc;
  uint32_t epoch;
  int idx;
} bs_flyer_t;

typedef struct {
  float phi, rho, v, speed, flip, a;
  int g;
} bs_particle_t;

typedef struct bs_scene {
  int mode;
  uint32_t rnd;
  float t;
  int host;            // BS_HOST_*

  uint16_t path[BS_PATH_MAX];
  int path_len;
  int reveal;          // how many path elements have landed in the path bar
  float reveal_at;
  int level_base;
  float cam_z, vel;

  bs_action_t queue[BS_QUEUE_MAX];
  int q_len;
  struct { bool active; bs_action_t a; float t0, dur; } act;
  struct { float z0, z1, dist, lock; } mv;

  bs_flyer_t flyers[BS_FLYERS_MAX];
  int n_flyers;
  uint8_t landed[BS_PATH_MAX / 8 + 1];   // bit i: level level_base + i has landed

  struct { bool on; float t0; int origin; float from; } lamp;
  struct { bool on; float t0; } lantern;
  float led_at;
  struct { float open_at, close_at; bool is_open; } book;
  struct { bool active; char text[40]; uint8_t color; float t0, dur; } toast;
  struct { bool active; uint16_t comps[BS_PATH_MAX]; int n; float t0, dur; } wipe;
  struct { bool active; int L; float t0; } lock_fx;
  struct { bool active; int L; uint16_t comp; float t0; } unlock_fx;
  float fov_kick;
  struct { float x, y, roll; } tilt, tilt_s;
  bool auto_tilt;      // gentle hand tremor when no IMU drives the tilt
  float bob_freq;
  uint32_t path_version, epoch;
  struct { uint32_t v; int n; int len; uint8_t bytes[BS_CONTENT_MAX]; } content;
  bs_particle_t particles[BS_PARTICLES];

  // firmware extras, all optional
  float splash_t0;     // boot splash start, < 0 for none
  bool show_fps;
  float fps;
  int free_kb;         // shown next to the fps when >= 0
  int battery_pct;     // < 0 when unknown
} bs_scene_t;

typedef struct {
  float vpx, vpy, roll, cam_z, K, ambient, lantern;
  const bs_scene_t *sc;
} bs_params_t;

// Build the wall textures, light maps and lookup tables. Call once at boot.
void bs_init_tables(void);

void bs_scene_init(bs_scene_t *sc, int mode, uint32_t seed);
void bs_set_mode(bs_scene_t *sc, int mode);

// Scene actions (queued; they play out over time).
void bs_connect(bs_scene_t *sc, int host_kind);
void bs_disconnect(bs_scene_t *sc);
void bs_enter(bs_scene_t *sc, uint16_t comp, int fast);
void bs_up(bs_scene_t *sc, int fast);
void bs_root(bs_scene_t *sc);
void bs_open(bs_scene_t *sc);
void bs_close(bs_scene_t *sc);
void bs_wait(bs_scene_t *sc, float seconds);
void bs_toggle_book(bs_scene_t *sc);
// Walk from wherever the queue will leave the scene to `target`. With
// `replace`, pending moves are dropped and the walk is planned from here.
void bs_navigate_to(bs_scene_t *sc, const uint16_t *target, int n, float interval, bool replace);
// Look up a file by its contents: dives to the folder that holds `bytes`.
void bs_goto_bytes(bs_scene_t *sc, const uint8_t *bytes, int len, float interval, bool replace);
void bs_toast(bs_scene_t *sc, const char *text, uint8_t color, float dur);
int bs_queue_pending(const bs_scene_t *sc);

void bs_update(bs_scene_t *sc, float dt);

// Rendering. bs_render() does everything; the split calls let the firmware
// render shaft rows on both cores.
void bs_render(bs_scene_t *sc, uint8_t *fb);
void bs_frame_params(const bs_scene_t *sc, bs_params_t *P);
void bs_render_shaft_rows(const bs_params_t *P, uint8_t *fb, int y0, int y1);
void bs_render_overlays(bs_scene_t *sc, const bs_params_t *P, uint8_t *fb);

// Encodings.
int bs_fanout(int mode);                                  // 256 or 4900
void bs_name(int mode, uint16_t comp, char out[3]);       // two characters + NUL
int bs_content_of(int mode, const uint16_t *path, int n, uint8_t *out, int max);
int bs_path_for(int mode, const uint8_t *bytes, int len, uint16_t *out, int max);
