#pragma once

#include <stdint.h>

// ST7789 panel in landscape, 240 x 135.
void display_init(void);
// Push one frame of palette indices (240 x 135 bytes) through the 16-entry
// RGB565 palette. Returns once `fb` has been read; the last strip may still
// be on the wire, so the caller can start drawing the next frame at once.
void display_push(const uint8_t *fb);
// Backlight, 0..255.
void display_set_brightness(uint8_t level);
