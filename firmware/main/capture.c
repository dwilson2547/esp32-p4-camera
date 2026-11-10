#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "sd_card.h"
#include "capture.h"

static const char *TAG = "capture";

#define DCIM_DIR        SD_MOUNT_POINT "/DCIM"
#define JPEG_QUALITY    90
#define JPEG_OUT_MAX    (3 * 1024 * 1024)   // generous for a 2.9 MP q90 4:2:2 frame

static jpeg_encoder_handle_t s_enc;
static jpeg_encode_cfg_t s_cfg;
static uint8_t *s_out;
static size_t s_out_cap;
static uint32_t s_out_len;
static int s_next_num = 1;

// Shots waiting for the card. The shutter only waits for the encode (~50 ms);
// each JPEG is copied into its own PSRAM block and queued, and the writer task
// drains the queue at whatever speed the card manages. A NULL item is a card test.
typedef struct { uint8_t *data; uint32_t len; } shot_t;
#define QUEUE_DEPTH     64
#define BUDGET_MAX      (16u * 1024 * 1024)
#define PSRAM_KEEP      (2u * 1024 * 1024)    // left free for everything else
static QueueHandle_t s_queue;
static size_t s_budget;
static volatile size_t s_queued_bytes;
static volatile int s_pending;
static volatile bool s_card_test;
static uint32_t s_last_len = 1024 * 1024;     // room to keep free for the next shot
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// Continue numbering after the highest IMG_nnnn.JPG already in DCIM.
static void find_next_number(void)
{
    DIR *d = opendir(DCIM_DIR);
    if (!d) return;
    struct dirent *e;
    int max = 0, n;
    while ((e = readdir(d)) != NULL)
        if (sscanf(e->d_name, "IMG_%4d.JPG", &n) == 1 && n > max) max = n;
    closedir(d);
    s_next_num = max + 1;
}

// Written the same way as the boot selftest (which has passed 4 MB every boot):
// 64 KB chunks copied into internal DMA RAM, so the SD controller never DMAs
// straight from the PSRAM JPEG buffer.
#define WRITE_CHUNK  (64 * 1024)
static uint8_t *s_chunk;

static bool write_file(const char *path, const uint8_t *data, uint32_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    setvbuf(f, NULL, _IONBF, 0);   // sd_card_fwrite's 512-byte pieces must reach FatFs as is
    size_t wr = 0;
    while (wr < len) {
        size_t n = len - wr < WRITE_CHUNK ? len - wr : WRITE_CHUNK;
        memcpy(s_chunk, data + wr, n);
        if (sd_card_fwrite(s_chunk, n, f) != n) break;
        wr += n;
    }
    bool ok = wr == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        ESP_LOGE(TAG, "writing %s failed (%u of %" PRIu32 " bytes)", path, (unsigned)wr, len);
        remove(path);
    }
    return ok;
}

static void writer_task(void *arg)
{
    for (;;) {
        shot_t shot;
        xQueueReceive(s_queue, &shot, portMAX_DELAY);
        if (!shot.data) {
            // Multi-block writes of growing size, then back to single-block for photos.
            static const size_t pieces[] = { 4 * 1024, 16 * 1024, 64 * 1024 };
            const size_t keep = sd_card_write_piece();
            for (int i = 0; i < 3; i++) {
                ESP_LOGI(TAG, "card test: 8 MB in %u KB pieces...", (unsigned)(pieces[i] / 1024));
                sd_card_set_write_piece(pieces[i]);
                if (sd_card_selftest(8) != ESP_OK) {
                    ESP_LOGE(TAG, "card test FAILED at %u KB pieces", (unsigned)(pieces[i] / 1024));
                    sd_card_remount();
                }
            }
            sd_card_set_write_piece(keep);
            ESP_LOGI(TAG, "card test done");
            s_card_test = false;
            continue;
        }
        char path[48];
        snprintf(path, sizeof path, DCIM_DIR "/IMG_%04d.JPG", s_next_num);
        int64_t t0 = esp_timer_get_time();
        bool ok = write_file(path, shot.data, shot.len);
        if (!ok && sd_card_remount() == ESP_OK) {
            // The card stopped answering mid-write; after a remount, try once more.
            ok = write_file(path, shot.data, shot.len);
        }
        int64_t t1 = esp_timer_get_time();
        free(shot.data);
        taskENTER_CRITICAL(&s_lock);
        s_queued_bytes -= shot.len;
        s_pending--;
        taskEXIT_CRITICAL(&s_lock);
        if (ok) {
            ESP_LOGI(TAG, "saved %s: %" PRIu32 " KB, write %.0f ms, %d still queued", path,
                     shot.len / 1024, (t1 - t0) / 1e3, s_pending);
            s_next_num++;
        } else {
            ESP_LOGE(TAG, "photo lost: SD card not writable");
        }
    }
}

esp_err_t capture_init(uint32_t width, uint32_t height)
{
    struct stat st;
    if (stat(SD_MOUNT_POINT, &st) != 0) return ESP_ERR_INVALID_STATE;
    if (stat(DCIM_DIR, &st) != 0 && mkdir(DCIM_DIR, 0775) != 0) {
        ESP_LOGE(TAG, "cannot create %s", DCIM_DIR);
        return ESP_FAIL;
    }
    find_next_number();

    jpeg_encode_engine_cfg_t ec = { .timeout_ms = 1000 };
    esp_err_t err = jpeg_new_encoder_engine(&ec, &s_enc);
    if (err != ESP_OK) return err;
    jpeg_encode_memory_alloc_cfg_t mc = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_out = jpeg_alloc_encoder_mem(JPEG_OUT_MAX, &mc, &s_out_cap);
    if (!s_out) return ESP_ERR_NO_MEM;
    s_cfg = (jpeg_encode_cfg_t){
        .width = width, .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = JPEG_QUALITY,
    };
    s_chunk = heap_caps_malloc(WRITE_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_chunk) return ESP_ERR_NO_MEM;
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_budget = free_ps > PSRAM_KEEP ? free_ps - PSRAM_KEEP : 0;
    if (s_budget > BUDGET_MAX) s_budget = BUDGET_MAX;
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(shot_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    xTaskCreate(writer_task, "sd_writer", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "ready: %" PRIu32 "x%" PRIu32 " JPEG q%d, next file IMG_%04d.JPG, %u KB shot buffer (PSRAM free %u KB)",
             width, height, JPEG_QUALITY, s_next_num, (unsigned)(s_budget / 1024), (unsigned)(free_ps / 1024));
    return ESP_OK;
}

void capture_card_test(void)
{
    if (!s_queue || s_card_test) return;
    shot_t t = { 0 };
    if (xQueueSend(s_queue, &t, 0) == pdTRUE) s_card_test = true;
}

bool capture_busy(void)
{
    if (!s_enc || !s_queue) return true;
    // Room for another shot about as big as the last one (with margin), and a queue slot.
    return s_queued_bytes + s_last_len + s_last_len / 2 > s_budget || uxQueueSpacesAvailable(s_queue) == 0;
}

int capture_pending(void) { return s_pending; }

esp_err_t capture_frame(const uint8_t *rgb565, size_t size)
{
    if (capture_busy()) return ESP_ERR_INVALID_STATE;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(s_enc, &s_cfg, rgb565, size, s_out, s_out_cap, &s_out_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
        return err;
    }
    s_last_len = s_out_len;
    if (s_queued_bytes + s_out_len > s_budget) {
        ESP_LOGW(TAG, "shot buffer full (%u KB queued), shot dropped", (unsigned)(s_queued_bytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    shot_t shot = { heap_caps_malloc(s_out_len, MALLOC_CAP_SPIRAM), s_out_len };
    if (!shot.data) {
        ESP_LOGW(TAG, "no PSRAM for a %" PRIu32 " KB shot, dropped", s_out_len / 1024);
        return ESP_ERR_NO_MEM;
    }
    memcpy(shot.data, s_out, s_out_len);
    taskENTER_CRITICAL(&s_lock);
    s_queued_bytes += shot.len;
    s_pending++;
    taskEXIT_CRITICAL(&s_lock);
    if (xQueueSend(s_queue, &shot, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_queued_bytes -= shot.len;
        s_pending--;
        taskEXIT_CRITICAL(&s_lock);
        free(shot.data);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "encoded + queued in %.1f ms (%" PRIu32 " KB, %d waiting for the card)",
             (esp_timer_get_time() - t0) / 1e3, s_out_len / 1024, s_pending);
    return ESP_OK;
}
