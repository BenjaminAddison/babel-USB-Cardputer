// Host-side check of the C port: plays the concept page's demo script and
// writes raw frames (BS_W x BS_H palette indices each) for chosen moments.
//
//   make -C firmware/test && ./firmware/test/host_render hex 1.5,4.6,9 out.raw
//   node design/tools/compare-c.js hex 1.5,4.6,9    (builds and diffs against the JS)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "babel_screen.h"

typedef struct { const char *cmd; const char *a; float b; } step_t;

// DEMO_SCRIPT from design/babel-screen.js
static const step_t DEMO[] = {
  { "wait", NULL, 2.4f },
  { "connect", NULL, 0 }, { "wait", NULL, 0.5f },
  { "goto", "Hello", 0.32f }, { "wait", NULL, 0.5f },
  { "open", NULL, 0 }, { "wait", NULL, 2.2f }, { "close", NULL, 0 }, { "wait", NULL, 0.5f },
  { "up", NULL, 0 }, { "wait", NULL, 0.45f },
  { "goto", "Help", 0.3f }, { "wait", NULL, 0.4f },
  { "open", NULL, 0 }, { "wait", NULL, 1.5f }, { "close", NULL, 0 }, { "wait", NULL, 0.5f },
  { "root", NULL, 0 }, { "wait", NULL, 0.9f },
  { "disconnect", NULL, 0 }, { "wait", NULL, 2.2f },
};
#define NDEMO (int)(sizeof DEMO / sizeof DEMO[0])

static int step_i = 0;
static float wait_until = 0;

static void director(bs_scene_t *sc) {
  if (sc->t < wait_until || bs_queue_pending(sc)) return;
  if (step_i >= NDEMO) step_i = 0;
  const step_t *s = &DEMO[step_i++];
  if (!strcmp(s->cmd, "wait")) wait_until = sc->t + s->b;
  else if (!strcmp(s->cmd, "connect")) bs_connect(sc, BS_HOST_MTP);
  else if (!strcmp(s->cmd, "disconnect")) bs_disconnect(sc);
  else if (!strcmp(s->cmd, "goto")) bs_goto_bytes(sc, (const uint8_t *)s->a, (int)strlen(s->a), s->b, false);
  else if (!strcmp(s->cmd, "open")) bs_open(sc);
  else if (!strcmp(s->cmd, "close")) bs_close(sc);
  else if (!strcmp(s->cmd, "up")) bs_up(sc, 0);
  else if (!strcmp(s->cmd, "root")) bs_root(sc);
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "hex";
  const char *times = argc > 2 ? argv[2] : "1,4,6.5,9,11.2,14,17,20";
  const char *out = argc > 3 ? argv[3] : "frames.raw";
  bs_init_tables();
  static bs_scene_t sc;
  bs_scene_init(&sc, !strcmp(mode, "b70") ? BS_MODE_B70 : BS_MODE_HEX, 3);
  static uint8_t fb[BS_W * BS_H];
  FILE *f = fopen(out, "wb");
  if (!f) { perror(out); return 1; }
  const double dt = 1.0 / 60;
  double t = 0;
  char *list = strdup(times);
  for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
    double target = atof(tok);
    while (t + dt / 2 < target) { director(&sc); bs_update(&sc, (float)dt); t += dt; }
    bs_render(&sc, fb);
    fwrite(fb, 1, sizeof fb, f);
  }
  fclose(f);
  // rough cost of one frame on this machine, for comparing changes
  clock_t c0 = clock();
  for (int i = 0; i < 200; i++) bs_render(&sc, fb);
  fprintf(stderr, "render: %.3f ms/frame (host)\n", (clock() - c0) * 1000.0 / CLOCKS_PER_SEC / 200);
  free(list);
  return 0;
}
