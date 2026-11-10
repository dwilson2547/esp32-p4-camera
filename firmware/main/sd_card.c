// microSD on the Waveshare ESP32-P4-Module-DEV-KIT TF slot.
//
// Wiring from the board schematic (ESP32-P4-Module-DEV-KIT.pdf, "MicroSD Card"
// block): SDMMC slot 0 IOMUX pins, 4-bit — CLK GPIO43, CMD GPIO44, D0 GPIO39,
// D1 GPIO40, D2 GPIO41, D3 GPIO42, 51k pull-ups on CMD/D0-D3. Card VDD is
// SD1_VDD, fed from the P4's on-chip LDO output VO4 (R80 0R; the ESP_3V3
// option R81 is not fitted). No card-detect line is wired to the P4.
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sd_card.h"

static const char *TAG = "sd_card";

#define SD_LDO_CHAN   4

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;

esp_err_t sd_card_mount(void)
{
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO VO4 for card power: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   // 40 MHz, 3.3 V signalling
    host.pwr_ctrl_handle = pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43; slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;  slot.d1 = GPIO_NUM_40; slot.d2 = GPIO_NUM_41; slot.d3 = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   // board has 51k; harmless extra

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   // never wipe the user's card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL)
            ESP_LOGE(TAG, "card found but no FAT filesystem mounted (not formatting it)");
        else
            ESP_LOGE(TAG, "card init failed: %s (card inserted?)", esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(pwr);
        return err;
    }
    s_pwr = pwr;
    sdmmc_card_print_info(stdout, s_card);

    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK)
        ESP_LOGI(TAG, "mounted at %s: %.2f GB total, %.2f GB free", SD_MOUNT_POINT,
                 total / 1e9, free_b / 1e9);
    return ESP_OK;
}

esp_err_t sd_card_remount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    if (s_pwr) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        s_pwr = NULL;
    }
    ESP_LOGW(TAG, "remounting the card");
    return sd_card_mount();
}

esp_err_t sd_card_selftest(void)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    const char *path = SD_MOUNT_POINT "/p4test.bin";
    const size_t chunk = 64 * 1024, total = 4 * 1024 * 1024;   // 4 MB, about a large JPEG or two
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rd = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf || !rd) { free(buf); free(rd); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < chunk; i++) buf[i] = (uint8_t)(i * 7 + 3);

    esp_err_t res = ESP_FAIL;
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "open %s for write failed", path); goto out; }
    int64_t t0 = esp_timer_get_time();
    for (size_t n = 0; n < total; n += chunk)
        if (fwrite(buf, 1, chunk, f) != chunk) { ESP_LOGE(TAG, "write failed at %u", (unsigned)n); fclose(f); goto out; }
    fflush(f); fsync(fileno(f)); fclose(f);
    int64_t t1 = esp_timer_get_time();

    f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s for read failed", path); goto out; }
    for (size_t n = 0; n < total; n += chunk) {
        if (fread(rd, 1, chunk, f) != chunk || memcmp(rd, buf, chunk) != 0) {
            ESP_LOGE(TAG, "read-back mismatch at %u", (unsigned)n); fclose(f); goto out;
        }
    }
    fclose(f);
    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "selftest OK: 4 MB write %.1f MB/s, read+verify %.1f MB/s",
             total / ((t1 - t0) / 1e6) / 1e6, total / ((t2 - t1) / 1e6) / 1e6);
    res = ESP_OK;
out:
    remove(path);
    free(buf); free(rd);
    return res;
}
