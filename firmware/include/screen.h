#pragma once

#include <stdint.h>

// Events from the MTP handlers (TinyUSB task) to the display task. Posting
// never blocks; if the queue is full the event is dropped and the screen
// catches up on the next one.
typedef enum {
    SCREEN_EV_CONNECT,      // OpenSession, or the bus resumed with a session open
    SCREEN_EV_DISCONNECT,   // CloseSession, bus suspended (cable pulled) or unmounted
    SCREEN_EV_ROOT,         // GetObjectHandles(root)
    SCREEN_EV_ENTER,        // GetObjectHandles(folder): a = depth of that folder's parent, b = folder index
    SCREEN_EV_OPEN,         // GetObject(`file`): a = path depth of the file, b = size in bytes
} screen_ev_type_t;

void screen_post(screen_ev_type_t type, uint32_t a, uint32_t b);
void screen_start(void);

// Shared with usb_mtp_impl.c.h
extern volatile uint32_t babel_activity;   // bumps on any MTP traffic: the activity LED
extern volatile int babel_mode;            // BS_MODE_HEX or BS_MODE_B70; read at OpenSession
extern volatile bool babel_session_open;

// Settings (naming mode, brightness) from NVS. Call before USB starts.
void app_load_settings(void);
