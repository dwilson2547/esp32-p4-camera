// microSD card on the dev kit's TF slot, mounted as FAT at SD_MOUNT_POINT.
#pragma once
#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

// Power the slot, mount the card and log its details. Never formats: a card
// that does not mount is left untouched and an error is returned.
esp_err_t sd_card_mount(void);

// Write, read back and delete a scratch file, logging throughput. Leaves the
// card as it found it.
esp_err_t sd_card_selftest(void);

// Unmount and mount again: recovers a card that stopped answering mid-write.
esp_err_t sd_card_remount(void);
