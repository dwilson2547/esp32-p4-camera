// On-screen controls drawn straight into the RGB888 DPI frame buffer, in the
// letterbox strips the camera picture does not cover.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SHUTTER_IDLE,      // white ring, white centre
    SHUTTER_PRESSED,   // finger on it: grey centre
    SHUTTER_SAVING,    // previous photo still being written: grey ring, dark centre
} shutter_state_t;

// Screen size and where the camera picture sits (its left edge and width), so
// the button can be placed clear of it.
void ui_init(uint8_t *fb, uint32_t lcd_w, uint32_t lcd_h, uint32_t pic_x, uint32_t pic_w);

void ui_draw_shutter(shutter_state_t st);

// Touch hit test in screen coordinates, with some slack around the button.
bool ui_shutter_hit(int x, int y);
