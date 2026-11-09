// The MTP side's `file` contents (babel_build_output, pulled out of
// usb_mtp_impl.c.h) for random walks, including reads from ancestor folders.
//
//   ./mtp_output_test             self-checks: contents -> path round trips
//   ./mtp_output_test --vectors   prints "mode path... : bytes..." lines that
//                                 design/tools/verify.js checks with BigInt maths
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "babel_screen.h"

uint8_t output_file[4096];
size_t output_file_len;
volatile int babel_mode;
#define BABEL_STACK_MAX 4096
uint16_t babel_stack[BABEL_STACK_MAX];
uint32_t babel_depth;
#include "mtp_output_extract.inc"

int main(int argc, char **argv) {
  bool vectors = argc > 1 && !strcmp(argv[1], "--vectors");
  int fails = 0, checks = 0;
  srand(12345);
  for (int mode = 0; mode < 2; mode++) {
    babel_mode = mode;
    for (int trial = 0; trial < (vectors ? 60 : 2000); trial++) {
      int depth = 1 + rand() % 40;
      for (int i = 0; i < depth; i++) babel_stack[i] = (uint16_t)(rand() % bs_fanout(mode));
      if (trial % 5 == 0) babel_stack[0] = 0;               // leading zero bytes
      if (trial % 7 == 0) babel_stack[depth - 1] = (uint16_t)(bs_fanout(mode) - 1);
      babel_depth = depth;
      int k = trial % 3 ? depth : rand() % (depth + 1);    // `file` from an ancestor, as with cached listings
      babel_build_output(k);
      if (vectors) {
        printf("%d", mode);
        for (int i = 0; i < k; i++) printf(" %u", babel_stack[i]);
        printf(" :");
        for (size_t i = 0; i < output_file_len; i++) printf(" %u", output_file[i]);
        printf("\n");
        continue;
      }
      uint16_t back[BS_PATH_MAX];
      int bn = bs_path_for(mode, output_file, (int)output_file_len, back, BS_PATH_MAX);
      checks++;
      if (bn != k || memcmp(back, babel_stack, k * sizeof(uint16_t))) { fails++; printf("round trip failed: mode %d depth %d\n", mode, k); }
    }
  }
  if (!vectors) printf(fails ? "FAILED %d of %d\n" : "%d round trips ok\n", fails ? fails : checks, checks);
  return fails != 0;
}
