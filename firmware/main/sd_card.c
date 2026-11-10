// microSD on the Waveshare ESP32-P4-Module-DEV-KIT TF slot.
//
// Wiring from the board schematic (ESP32-P4-Module-DEV-KIT.pdf, "MicroSD Card"
// block): SDMMC slot 0 IOMUX pins, 4-bit — CLK GPIO43, CMD GPIO44, D0 GPIO39,
// D1 GPIO40, D2 GPIO41, D3 GPIO42, 51k pull-ups on CMD/D0-D3. Card VDD is
// SD1_VDD, fed from the P4's on-chip LDO output VO4 (R80 0R; the ESP_3V3
// option R81 is not fitted). No card-detect line is wired to the P4.
//
// SD_EXTERNAL_SPI (sd_card.h) selects instead an external SPI microSD breakout
// on the 40-pin header, powered from the header's 3V3 rather than VO4:
//   CS GPIO23 (pin 7), CLK GPIO21 (pin 11), MOSI GPIO22 (pin 12),
//   MISO GPIO20 (pin 13), 3V3 pin 1, GND pin 9.
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
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "soc/sdmmc_struct.h"
#include "sd_card.h"

static const char *TAG = "sd_card";

#define SD_LDO_CHAN   4

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;
static bool s_spi_bus;

static const esp_vfs_fat_sdmmc_mount_config_t s_mc = {
    .format_if_mount_failed = false,   // never wipe the user's card
    .max_files = 4,
    .allocation_unit_size = 16 * 1024,
};

static void log_mounted(void)
{
    sdmmc_card_print_info(stdout, s_card);
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK)
        ESP_LOGI(TAG, "mounted at %s: %.2f GB total, %.2f GB free", SD_MOUNT_POINT,
                 total / 1e9, free_b / 1e9);
}

#if SD_EXTERNAL_SPI
esp_err_t sd_card_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    // 20 MHz. Asking for 40 MHz changes nothing: the card (SanDisk SA08G) does not
    // enter high-speed mode over SPI, so IDF falls back to the 20 MHz default.
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    if (!s_spi_bus) {
        spi_bus_config_t bus = {
            .mosi_io_num = GPIO_NUM_22, .miso_io_num = GPIO_NUM_20, .sclk_io_num = GPIO_NUM_21,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 64 * 1024,
        };
        esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
        if (e != ESP_OK) { ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(e)); return e; }
        // The card's MISO floats between replies; the breakout may have no pull-ups.
        gpio_pullup_en(GPIO_NUM_20); gpio_pullup_en(GPIO_NUM_22); gpio_pullup_en(GPIO_NUM_23);
        s_spi_bus = true;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = GPIO_NUM_23;
    slot.host_id = SPI2_HOST;
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &s_mc, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "external SPI card: %s (wiring? card inserted? FAT?)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "external SPI slot");
    log_mounted();
    return ESP_OK;
}
#else
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
    // 40 MHz (upstream default). 20 MHz was tried while chasing write timeouts and
    // failed more often, not less.
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43; slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;  slot.d1 = GPIO_NUM_40; slot.d2 = GPIO_NUM_41; slot.d3 = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   // board has 51k; harmless extra

    err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &s_mc, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL)
            ESP_LOGE(TAG, "card found but no FAT filesystem mounted (not formatting it)");
        else
            ESP_LOGE(TAG, "card init failed: %s (card inserted?)", esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(pwr);
        return err;
    }
    s_pwr = pwr;
    // IDF 6.0.3 never programs FIFOTH.dma_multiple_transaction_size, so the SDMMC
    // DMA runs 1-byte bursts. Upstream (esp-idf 2e02ed8, master) now defaults the
    // P4 to 16-byte bursts (field value 3). Multi-sector writes here time out with
    // the card left waiting for data, so set it the same way.
    SDMMC.fifoth.dma_multiple_transaction_size = 3;
    log_mounted();
    return ESP_OK;
}
#endif

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

// xorshift32 bytes: different for every chunk, repeatable for the read-back check.
static void fill_random(uint8_t *buf, size_t len, uint32_t x)
{
    x |= 1;
    for (size_t i = 0; i < len; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; buf[i] = (uint8_t)x; }
}

static size_t s_piece = SD_EXTERNAL_SPI ? 64 * 1024 : 512;
void sd_card_set_write_piece(size_t bytes) { s_piece = bytes < 512 ? 512 : bytes; }
size_t sd_card_write_piece(void) { return s_piece; }

size_t sd_card_fwrite(const void *buf, size_t len, FILE *f)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < s_piece ? len - done : s_piece;
        if (fwrite(p + done, 1, n, f) != n) break;
        done += n;
    }
    return done;
}

esp_err_t sd_card_selftest(unsigned total_mb)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    const char *path = SD_MOUNT_POINT "/p4test.bin";
    const size_t chunk = 64 * 1024, total = (size_t)total_mb * 1024 * 1024;
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rd = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf || !rd) { free(buf); free(rd); return ESP_ERR_NO_MEM; }
    // Pseudo-random data: photo writes failed on some content while a simple
    // repeating pattern always passed.
    const uint32_t seed = 0x9e3779b9 ^ (uint32_t)esp_timer_get_time();
#define FILL(c) fill_random(buf, chunk, seed + (uint32_t)(c))

    esp_err_t res = ESP_FAIL;
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "open %s for write failed", path); goto out; }
    setvbuf(f, NULL, _IONBF, 0);
    int64_t t0 = esp_timer_get_time();
    for (size_t n = 0; n < total; n += chunk)
        if ((FILL(n), sd_card_fwrite(buf, chunk, f)) != chunk) { ESP_LOGE(TAG, "write failed at %u (%u KB pieces)", (unsigned)n, (unsigned)(s_piece / 1024)); fclose(f); goto out; }
    fflush(f); fsync(fileno(f)); fclose(f);
    int64_t t1 = esp_timer_get_time();

    f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s for read failed", path); goto out; }
    for (size_t n = 0; n < total; n += chunk) {
        FILL(n);
        if (fread(rd, 1, chunk, f) != chunk || memcmp(rd, buf, chunk) != 0) {
            ESP_LOGE(TAG, "read-back mismatch at %u", (unsigned)n); fclose(f); goto out;
        }
    }
    fclose(f);
    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "selftest OK: %u MB, %u KB pieces, write %.1f MB/s, read+verify %.1f MB/s",
             total_mb, (unsigned)(s_piece / 1024), total / ((t1 - t0) / 1e6) / 1e6, total / ((t2 - t1) / 1e6) / 1e6);
    res = ESP_OK;
out:
    remove(path);
    free(buf); free(rd);
    return res;
}
