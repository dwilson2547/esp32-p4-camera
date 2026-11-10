// Still capture: hardware JPEG encode of a camera frame, written to the SD card
// as SD_MOUNT_POINT/DCIM/IMG_nnnn.JPG by a background task. Shots queue in PSRAM
// so the shutter is free again as soon as the encode finishes.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Set up the encoder and writer task for width x height RGB565 frames. Fails if
// the SD card is not mounted.
esp_err_t capture_init(uint32_t width, uint32_t height);

// True when the PSRAM shot buffer has no room for another photo (or capture is
// not set up); a new shot is refused. Photos still being written do not block.
bool capture_busy(void);

// Photos encoded but not yet on the card.
int capture_pending(void);

// Encode the frame now (blocks for the encode only) and queue it for writing.
// The frame buffer can be reused as soon as this returns.
esp_err_t capture_frame(const uint8_t *rgb565, size_t size);

// Queue the SD card write/read selftest on the writer task, behind any photos
// waiting (diagnostic). Ignored while one is already queued or running.
void capture_card_test(void);

// Write a raw sensor frame to SD_MOUNT_POINT/DCIM/RAW_nnnn.RAW now, blocking
// until it is on the card (same numbering as the JPEGs). Refused while photos
// are still queued for the writer task.
esp_err_t capture_save_raw(const uint8_t *data, size_t len);
