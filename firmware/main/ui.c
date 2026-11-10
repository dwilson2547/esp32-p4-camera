#include <string.h>
#include "esp_cache.h"
#include "esp_log.h"
#include "ui.h"

static const char *TAG = "ui";

#define BTN_R_OUTER   34
#define BTN_RING      4
#define BTN_R_INNER   26
#define BTN_R_DOT     8      // "still writing" dot
#define BTN_MARGIN    14     // from the bottom and right screen edges
#define HIT_SLACK     16

static uint8_t *s_fb;
static uint32_t s_w, s_h;
static int s_cx, s_cy;

void ui_init(uint8_t *fb, uint32_t lcd_w, uint32_t lcd_h, uint32_t pic_x, uint32_t pic_w)
{
    s_fb = fb; s_w = lcd_w; s_h = lcd_h;
    s_cx = (int)lcd_w - BTN_MARGIN - BTN_R_OUTER;
    s_cy = (int)lcd_h - BTN_MARGIN - BTN_R_OUTER;
    if (s_cx - BTN_R_OUTER < (int)(pic_x + pic_w))
        ESP_LOGW(TAG, "shutter overlaps the picture (right strip only %d px)", (int)(lcd_w - pic_x - pic_w));
    ESP_LOGI(TAG, "shutter button at (%d,%d) r=%d", s_cx, s_cy, BTN_R_OUTER);
}

void ui_draw_shutter(shutter_state_t st)
{
    if (!s_fb) return;
    const uint8_t ring = st == SHUTTER_SAVING ? 0x60 : 0xff;
    const uint8_t centre = (st == SHUTTER_IDLE || st == SHUTTER_WRITING) ? 0xff : st == SHUTTER_PRESSED ? 0x90 : 0x30;
    const int r_ring_in = BTN_R_OUTER - BTN_RING;
    const int y0 = s_cy - BTN_R_OUTER, y1 = s_cy + BTN_R_OUTER;
    for (int y = y0; y <= y1; y++) {
        uint8_t *row = s_fb + (size_t)y * s_w * 3;
        for (int x = s_cx - BTN_R_OUTER; x <= s_cx + BTN_R_OUTER; x++) {
            int dx = x - s_cx, dy = y - s_cy, d2 = dx * dx + dy * dy;
            uint8_t v;
            if (d2 > BTN_R_OUTER * BTN_R_OUTER) continue;           // outside: leave black
            else if (d2 >= r_ring_in * r_ring_in) v = ring;
            else if (d2 > BTN_R_INNER * BTN_R_INNER) v = 0;          // gap between ring and centre
            else if (st == SHUTTER_WRITING && d2 <= BTN_R_DOT * BTN_R_DOT) v = 0x30;
            else v = centre;
            memset(row + x * 3, v, 3);   // grey levels only, so RGB/BGR order does not matter
        }
    }
    // Write back the rows touched. Lines shared with the picture area are clean
    // (the CPU never writes there), so only the button's own bytes are flushed.
    esp_cache_msync(s_fb + (size_t)y0 * s_w * 3, (size_t)(y1 - y0 + 1) * s_w * 3,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

bool ui_shutter_hit(int x, int y)
{
    if (!s_fb) return false;
    int r = BTN_R_OUTER + HIT_SLACK, dx = x - s_cx, dy = y - s_cy;
    return dx * dx + dy * dy <= r * r;
}
