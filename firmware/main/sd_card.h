// microSD card on the dev kit's TF slot, mounted as FAT at SD_MOUNT_POINT.
#pragma once
#include <stdio.h>
#include <stddef.h>
#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

// 1: external SPI microSD breakout on the 40-pin header (see sd_card.c for
// wiring), multi-block writes. 0: the board's own TF slot (SDMMC 4-bit),
// single-block writes because multi-block ones hang there.
#define SD_EXTERNAL_SPI 1

// Power the slot, mount the card and log its details. Never formats: a card
// that does not mount is left untouched and an error is returned.
esp_err_t sd_card_mount(void);

// Write, read back and delete a scratch file of total_mb MB of random data,
// logging throughput. Leaves the card as it found it.
esp_err_t sd_card_selftest(unsigned total_mb);

// fwrite for files on the card, in pieces of at most the current write size
// (default 512 bytes). Multi-block writes (CMD25) intermittently hang on this
// board under IDF 6.0.3; with 512-byte pieces FatFs issues only single-block
// writes (CMD24). The FILE must be unbuffered (setvbuf _IONBF) or stdio
// regroups the pieces.
size_t sd_card_fwrite(const void *buf, size_t len, FILE *f);

// Largest piece sd_card_fwrite hands to FatFs: 512 for single-block writes,
// larger to allow multi-block writes of up to that size.
void sd_card_set_write_piece(size_t bytes);
size_t sd_card_write_piece(void);

// Unmount and mount again: recovers a card that stopped answering mid-write.
esp_err_t sd_card_remount(void);
