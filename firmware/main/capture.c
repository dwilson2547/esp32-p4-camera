#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
static volatile bool s_busy;
static SemaphoreHandle_t s_write_req;

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

static bool write_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t wr = 0;
    while (wr < s_out_len) {
        size_t n = s_out_len - wr < WRITE_CHUNK ? s_out_len - wr : WRITE_CHUNK;
        memcpy(s_chunk, s_out + wr, n);
        if (fwrite(s_chunk, 1, n, f) != n) break;
        wr += n;
    }
    bool ok = wr == s_out_len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        ESP_LOGE(TAG, "writing %s failed (%u of %" PRIu32 " bytes)", path, (unsigned)wr, s_out_len);
        remove(path);
    }
    return ok;
}

static void writer_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_write_req, portMAX_DELAY);
        char path[48];
        snprintf(path, sizeof path, DCIM_DIR "/IMG_%04d.JPG", s_next_num);
        int64_t t0 = esp_timer_get_time();
        bool ok = write_file(path);
        if (!ok && sd_card_remount() == ESP_OK) {
            // The card stopped answering mid-write; after a remount, try once more.
            ok = write_file(path);
        }
        int64_t t1 = esp_timer_get_time();
        if (ok) {
            ESP_LOGI(TAG, "saved %s: %" PRIu32 " KB, write %.0f ms", path, s_out_len / 1024, (t1 - t0) / 1e3);
            s_next_num++;
        } else {
            ESP_LOGE(TAG, "photo lost: SD card not writable");
        }
        s_busy = false;
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
    s_write_req = xSemaphoreCreateBinary();
    xTaskCreate(writer_task, "sd_writer", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "ready: %" PRIu32 "x%" PRIu32 " JPEG q%d, next file IMG_%04d.JPG", width, height, JPEG_QUALITY, s_next_num);
    return ESP_OK;
}

bool capture_busy(void) { return s_busy || !s_enc; }

esp_err_t capture_frame(const uint8_t *rgb565, size_t size)
{
    if (capture_busy()) return ESP_ERR_INVALID_STATE;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(s_enc, &s_cfg, rgb565, size, s_out, s_out_cap, &s_out_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "encoded in %.1f ms", (esp_timer_get_time() - t0) / 1e3);
    s_busy = true;
    xSemaphoreGive(s_write_req);
    return ESP_OK;
}
