// microSD card on the dev kit's TF slot, mounted as FAT at SD_MOUNT_POINT.
#pragma once
#include <stdio.h>
#include <stddef.h>
#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

// Power the slot, mount the card and log its details. Never formats: a card
// that does not mount is left untouched and an error is returned.
esp_err_t sd_card_mount(void);

// Write, read back and delete a scratch file of total_mb MB of random data,
// logging throughput. Leaves the card as it found it.
esp_err_t sd_card_selftest(unsigned total_mb);

// fwrite for files on the card. Multi-block writes (CMD25) intermittently hang
// on this board under IDF 6.0.3, so this writes one 512-byte sector per call:
// FatFs then issues single-block writes (CMD24) for the data. The FILE must be
// unbuffered (setvbuf _IONBF) or stdio regroups the pieces.
size_t sd_card_fwrite(const void *buf, size_t len, FILE *f);

// Unmount and mount again: recovers a card that stopped answering mid-write.
esp_err_t sd_card_remount(void);
