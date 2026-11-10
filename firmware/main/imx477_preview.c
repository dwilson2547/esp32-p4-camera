// Live camera preview: IMX477 (Raspberry Pi HQ Camera, MIPI-CSI) -> ISP (RGB565) -> PPA scale + RGB888
// conversion -> DPI frame buffer of the 800x480 Pi-protocol DSI display.
//
// Camera and display share one I2C bus (GPIO 7/8): the display MCU at 0x45, the
// touch controller at 0x38 and the IMX477 at 0x1a. The bus is created here and
// handed to both drivers so neither tries to claim the pins a second time.
#include <fcntl.h>
#include <string.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "driver/i2c_master.h"
#include "driver/ppa.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_rpi_dsi.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "freertos/semphr.h"
#include "sd_card.h"
#include "capture.h"
#include "ui.h"

// Shutter button: a touch that starts on it asks the main loop for a photo.
static volatile bool s_shot_requested;
static volatile bool s_button_down;
static volatile bool s_card_test_requested;   // diagnostic: tap the top-left corner

static void touch_task(void *arg)
{
    esp_lcd_touch_handle_t tp = arg;
    bool was_down = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_lcd_touch_point_data_t pt[1];
        uint8_t n = 0;
        if (esp_lcd_touch_read_data(tp) != ESP_OK) continue;
        esp_lcd_touch_get_data(tp, pt, &n, 1);
        bool down = n > 0;
        if (down && !was_down) {
            bool hit = ui_shutter_hit(pt[0].x, pt[0].y);
            ESP_LOGI("touch", "press at (%u,%u)%s", pt[0].x, pt[0].y, hit ? ": shutter" : "");
            if (hit) { s_shot_requested = true; s_button_down = true; }
            else if (pt[0].x < 150 && pt[0].y < 150) s_card_test_requested = true;
        }
        if (!down) s_button_down = false;
        was_down = down;
    }
}

static bool ppa_done_cb(ppa_client_handle_t c, ppa_event_data_t *e, void *user_data)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_data, &woken);
    return woken == pdTRUE;
}

// Configure the PPA to read every `skip`-th row of the camera frame: the row
// stride becomes skip * width, so the 2D DMA steps over the rows in between and
// the PPA only ever sees cam_h / skip rows. The vertical scale grows to match.
static void set_row_skip(ppa_srm_oper_config_t *op, uint32_t cam_w, uint32_t cam_h, uint32_t skip,
                         float s, uint32_t out_h)
{
    op->in.pic_w = cam_w * skip;
    op->in.pic_h = cam_h / skip;
    op->in.block_w = cam_w;
    op->in.block_h = cam_h / skip;
    op->scale_y = (float)out_h / op->in.block_h;
    op->scale_y = (float)(int)(op->scale_y * 16) / 16.0f;   // 1/16 steps, round down
    (void)s;
}

static const char *TAG = "imx477_preview";

#define I2C_SDA_GPIO   7
#define I2C_SCL_GPIO   8
#define SCCB_FREQ_HZ   100000
#define BUFFER_COUNT   3   // one with the PPA, one being filled, one spare

void app_main(void)
{
    // Shared I2C bus
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = I2C_SDA_GPIO, .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));

    // Display
    esp_lcd_rpi_dsi_config_t dcfg = ESP_LCD_RPI_DSI_DEFAULT_CONFIG();
    dcfg.i2c.bus = bus;
    esp_lcd_rpi_dsi_handle_t display = NULL;
    ESP_ERROR_CHECK(esp_lcd_rpi_dsi_start(&dcfg, &display));
    esp_lcd_panel_handle_t panel = esp_lcd_rpi_dsi_get_panel(display);
    void *fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb));
    const uint32_t lcd_w = dcfg.timing.h_size, lcd_h = dcfg.timing.v_size;
    const size_t fb_size = (size_t)lcd_w * lcd_h * 3;
    memset(fb, 0, fb_size);  // black letterbox around the picture
    ESP_ERROR_CHECK(esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
    ESP_LOGI(TAG, "display up: %" PRIu32 "x%" PRIu32 " RGB888", lcd_w, lcd_h);

    // SD card: bring-up check only for now. A missing or unreadable card is
    // logged and the preview runs anyway.
    bool sd_ok = sd_card_mount() == ESP_OK;
    // No boot selftest: with single-block writes it held the preview back for
    // seconds (much longer on slow cards). Tap the top-left corner to test.
    esp_lcd_touch_handle_t touch = esp_lcd_rpi_dsi_get_touch(display);
    if (touch) xTaskCreate(touch_task, "touch", 3072, touch, 4, NULL);
    else ESP_LOGW(TAG, "no touch controller: shutter disabled");

    // Camera
    const esp_video_init_csi_config_t csi_config[] = {{
        .sccb_config = { .init_sccb = false, .i2c_handle = bus, .freq = SCCB_FREQ_HZ },
        .reset_pin = -1,
        .pwdn_pin = -1,
    }};
    const esp_video_init_config_t cam_config = { .csi = csi_config };
    ESP_ERROR_CHECK(esp_video_init(&cam_config));

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) { ESP_LOGE(TAG, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME); return; }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format fmt = { .type = type };
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_G_FMT, &fmt) == 0 ? ESP_OK : ESP_FAIL);
    const uint32_t cam_w = fmt.fmt.pix.width, cam_h = fmt.fmt.pix.height, fourcc = fmt.fmt.pix.pixelformat;
    ESP_LOGI(TAG, "camera format %" PRIu32 "x%" PRIu32 " fourcc=%c%c%c%c", cam_w, cam_h,
             (char)(fourcc & 0xff), (char)((fourcc >> 8) & 0xff), (char)((fourcc >> 16) & 0xff), (char)(fourcc >> 24));
    if (fourcc != V4L2_PIX_FMT_RGB565) {
        ESP_LOGE(TAG, "expected RGB565 from the ISP, got something else; stopping");
        return;
    }

    struct v4l2_requestbuffers req = { .count = BUFFER_COUNT, .type = type, .memory = V4L2_MEMORY_MMAP };
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_REQBUFS, &req) == 0 ? ESP_OK : ESP_FAIL);
    uint8_t *cam_buf[BUFFER_COUNT];
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QUERYBUF, &b) == 0 ? ESP_OK : ESP_FAIL);
        cam_buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        ESP_ERROR_CHECK(ioctl(fd, VIDIOC_QBUF, &b) == 0 ? ESP_OK : ESP_FAIL);
    }
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_STREAMON, &type) == 0 ? ESP_OK : ESP_FAIL);

    // Fit the camera frame inside the panel. PPA scale factors have 1/16 steps,
    // so round down to one.
    float s = (float)lcd_w / cam_w;
    if ((float)lcd_h / cam_h < s) s = (float)lcd_h / cam_h;
    s = (float)(int)(s * 16) / 16.0f;
    const uint32_t out_w = (uint32_t)(cam_w * s), out_h = (uint32_t)(cam_h * s);
    ESP_LOGI(TAG, "scale %.4f -> %" PRIu32 "x%" PRIu32 " centred", s, out_w, out_h);

    SemaphoreHandle_t ppa_sem = xSemaphoreCreateBinary();
    ppa_client_handle_t ppa;
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
    ESP_ERROR_CHECK(ppa_register_client(&pc, &ppa));
    ppa_event_callbacks_t cbs = { .on_trans_done = ppa_done_cb };
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(ppa, &cbs));
    ppa_srm_oper_config_t op = {
        .in = { .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
        .out = { .buffer = fb, .buffer_size = fb_size, .pic_w = lcd_w, .pic_h = lcd_h,
                 .block_offset_x = (lcd_w - out_w) / 2, .block_offset_y = (lcd_h - out_h) / 2,
                 .srm_cm = PPA_SRM_COLOR_MODE_RGB888 },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = s, .scale_y = s,
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = ppa_sem,
    };

    // --- Startup benchmark on one real frame -------------------------------
    const int type_ = type; (void)type_;
    struct v4l2_buffer b0 = { .type = type, .memory = V4L2_MEMORY_MMAP };
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_DQBUF, &b0) == 0 ? ESP_OK : ESP_FAIL);
    op.in.buffer = cam_buf[b0.index];
    // Sensor frame period, for choosing a row skip that keeps up.
    struct v4l2_buffer bt = { .type = type, .memory = V4L2_MEMORY_MMAP };
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_DQBUF, &bt) == 0 ? ESP_OK : ESP_FAIL);
    int64_t tq = esp_timer_get_time();
    ioctl(fd, VIDIOC_QBUF, &bt);
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_DQBUF, &bt) == 0 ? ESP_OK : ESP_FAIL);
    ioctl(fd, VIDIOC_QBUF, &bt);
    ESP_ERROR_CHECK(ioctl(fd, VIDIOC_DQBUF, &bt) == 0 ? ESP_OK : ESP_FAIL);
    const float frame_ms = (esp_timer_get_time() - tq) / 2e3f;
    ioctl(fd, VIDIOC_QBUF, &bt);
    ESP_LOGI(TAG, "sensor frame period %.1f ms (%.1f fps)", frame_ms, 1000.0f / frame_ms);

    float skip_ms[5] = { 0 };
    for (uint32_t skip = 1; skip <= 4; skip *= 2) {
        set_row_skip(&op, cam_w, cam_h, skip, s, out_h);
        int64_t t = esp_timer_get_time();
        for (int i = 0; i < 3; i++) ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa, &op));
        skip_ms[skip] = (esp_timer_get_time() - t) / 3e3f;
        ESP_LOGI(TAG, "bench: every %" PRIu32 " row(s): in %" PRIu32 "x%" PRIu32 " scale %.4fx%.4f -> %.1f ms",
                 skip, op.in.block_w, op.in.block_h, op.scale_x, op.scale_y, skip_ms[skip]);
    }
    {   // Quarter-area block, full stride: does PPA time follow pixels read?
        set_row_skip(&op, cam_w, cam_h, 1, s, out_h);
        op.in.block_w = cam_w / 2; op.in.block_h = cam_h / 2; op.scale_y = s;
        int64_t t = esp_timer_get_time();
        for (int i = 0; i < 3; i++) ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa, &op));
        ESP_LOGI(TAG, "bench: quarter block %" PRIu32 "x%" PRIu32 " -> %.1f ms", op.in.block_w, op.in.block_h,
                 (esp_timer_get_time() - t) / 3e3f);
    }
    ioctl(fd, VIDIOC_QBUF, &b0);

    // Least skipping that still fits in one sensor frame (with 2 ms margin).
    uint32_t skip = 4;
    for (uint32_t k = 1; k <= 4; k *= 2) {
        if (skip_ms[k] + 2.0f <= frame_ms) { skip = k; break; }
    }
    set_row_skip(&op, cam_w, cam_h, skip, s, out_h);
    memset(fb, 0, fb_size);
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    ui_init(fb, lcd_w, lcd_h, (lcd_w - out_w) / 2, out_w);
    ESP_LOGI(TAG, "preview: every %" PRIu32 " row(s), %" PRIu32 " rows in, scale %.4fx%.4f",
             skip, op.in.block_h, op.scale_x, op.scale_y);

    // --- Pipelined loop -----------------------------------------------------
    // The PPA works on frame N while the camera fills the next buffer; frame N's
    // buffer goes back to the driver only once the PPA has finished with it.
    op.mode = PPA_TRANS_MODE_NON_BLOCKING;
    // The done callback also fires for the blocking benchmark runs above, which
    // leaves the semaphore given; clear it or the first wait would not wait.
    xSemaphoreTake(ppa_sem, 0);
    int in_flight = -1;
    const bool can_capture = sd_ok && capture_init(cam_w, cam_h) == ESP_OK;
    if (!can_capture) ESP_LOGW(TAG, "photo capture unavailable (SD card not mounted?)");
    const size_t frame_bytes = (size_t)cam_w * cam_h * 2;
    int flash_frames = 0;
    int shutter_drawn = -1;   // state last drawn; -1 forces a redraw
    int frames = 0;
    int64_t t0 = esp_timer_get_time(), t_wait = 0, t_ppa_wait = 0;
    for (;;) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
        int64_t ta = esp_timer_get_time();
        if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) { ESP_LOGE(TAG, "DQBUF failed"); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int64_t tb = esp_timer_get_time();
        t_wait += tb - ta;
        if (in_flight >= 0) {
            xSemaphoreTake(ppa_sem, portMAX_DELAY);
            struct v4l2_buffer done = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = in_flight };
            ioctl(fd, VIDIOC_QBUF, &done);
        }
        t_ppa_wait += esp_timer_get_time() - tb;
        in_flight = -1;

        if (s_shot_requested) {
            s_shot_requested = false;
            if (!can_capture) {
                ESP_LOGW(TAG, "shutter: no SD card");
            } else if (capture_busy()) {
                ESP_LOGW(TAG, "shutter: still saving the last photo");
            } else if (capture_frame(cam_buf[b.index], frame_bytes) == ESP_OK) {
                // White flash for a few frames to confirm the shot.
                memset(fb, 0xff, fb_size);
                esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                flash_frames = 4;
            }
        }
        if (s_card_test_requested) {
            s_card_test_requested = false;
            if (can_capture) capture_card_test();
        }
        if (flash_frames > 0) {
            ioctl(fd, VIDIOC_QBUF, &b);
            if (--flash_frames == 0) {
                memset(fb, 0, fb_size);   // back to the black letterbox
                esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                shutter_drawn = -1;
            }
            continue;
        }
        if (can_capture) {
            shutter_state_t st = capture_busy() ? SHUTTER_SAVING
                               : s_button_down ? SHUTTER_PRESSED : SHUTTER_IDLE;
            if ((int)st != shutter_drawn) { ui_draw_shutter(st); shutter_drawn = st; }
        }

        op.in.buffer = cam_buf[b.index];
        esp_err_t e = ppa_do_scale_rotate_mirror(ppa, &op);
        if (e != ESP_OK) {
            // Nothing is in flight now (the previous job was waited for above).
            ESP_LOGE(TAG, "PPA failed: %s", esp_err_to_name(e));
            ioctl(fd, VIDIOC_QBUF, &b);
            in_flight = -1;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        in_flight = b.index;
        if (++frames == 60) {
            int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "%.1f fps (per frame: camera wait %.1f ms, ppa wait %.1f ms)",
                     frames * 1e6 / (now - t0), t_wait / 1e3 / frames, t_ppa_wait / 1e3 / frames);
            frames = 0; t0 = now; t_wait = 0; t_ppa_wait = 0;
        }
    }
}
