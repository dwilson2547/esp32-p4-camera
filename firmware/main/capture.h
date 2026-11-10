// Still capture: hardware JPEG encode of a camera frame, written to the SD card
// as SD_MOUNT_POINT/DCIM/IMG_nnnn.JPG by a background task.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Set up the encoder and writer task for width x height RGB565 frames. Fails if
// the SD card is not mounted.
esp_err_t capture_init(uint32_t width, uint32_t height);

// True while the previous photo is still being written; a new shot is refused.
bool capture_busy(void);

// Encode the frame now (blocks for the encode only) and queue it for writing.
// The frame buffer can be reused as soon as this returns.
esp_err_t capture_frame(const uint8_t *rgb565, size_t size);

// Run the SD card write/read selftest on the writer task, alongside the live
// preview (diagnostic). Ignored while a photo is being saved.
void capture_card_test(void);
