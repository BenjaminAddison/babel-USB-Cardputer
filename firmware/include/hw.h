#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

// Internal I2C bus, shared by the keyboard, IMU and codec. Only the display
// task talks on it, so there is no locking beyond the driver's own.
i2c_master_bus_handle_t i2c_bus(void);

// ---------------------------------------------------------------- keyboard
// Key codes: printable ASCII as-is, plus these.
enum {
    KEY_NONE = 0,
    KEY_BKSP = 0x08,
    KEY_TAB = 0x09,
    KEY_ENTER = 0x0A,
    KEY_ESC = 0x1B,
    KEY_UP = 0x80,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_DEL,
    KEY_FN,     // modifier presses are reported too, so they can act as buttons
    KEY_SHIFT,
    KEY_CTRL,
    KEY_OPT,
    KEY_ALT,
    KEY_BTN_A,  // the G0 button on the side
};
typedef struct {
    uint8_t code;
    bool fn, shift, ctrl, opt, alt;   // modifiers held when the key went down
} key_event_t;

bool keyboard_init(void);
// Next key press, if any. Call often (it polls the TCA8418 and BtnA).
bool keyboard_poll(key_event_t *ev);

// --------------------------------------------------------------------- imu
bool imu_init(void);
// Accelerometer in g, board axes. Returns false if the IMU is absent.
bool imu_read_accel(float *ax, float *ay, float *az);

// ----------------------------------------------------------------- battery
void battery_init(void);
// Battery voltage in mV (0 if unknown) and a rough level in percent.
int battery_mv(void);
int battery_percent(int mv);

// ------------------------------------------------------------------- sound
bool sound_init(void);
void sound_set_enabled(bool on);
// Short effects, mixed and played from a background task.
void sound_tick(void);       // a folder locks
void sound_page(void);       // the book opens
void sound_chime(void);      // a host connects
