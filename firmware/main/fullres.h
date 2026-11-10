// Full-resolution raw still (feasibility spike): stop the preview, switch the
// IMX477 to 4056x3040 RAW10, capture one frame with the ISP bypassed, write it
// to the card as DCIM/RAW_nnnn.RAW, then bring the preview back. Every step is
// timed in the log. The preview is frozen for the whole capture, including the
// card write.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define FULLRES_BUF_ALIGN   256   // covers the PSRAM cache line the CSI driver checks
#define FULLRES_ALIGN(n)    (((n) + FULLRES_BUF_ALIGN - 1) & ~(size_t)(FULLRES_BUF_ALIGN - 1))
// 4056x3040 RAW10, MIPI-packed (4 px in 5 bytes)
#define FULLRES_W           4056
#define FULLRES_H           3040
#define FULLRES_BYTES       ((size_t)FULLRES_W * FULLRES_H * 10 / 8)
#define FULLRES_BUF_BYTES   FULLRES_ALIGN(FULLRES_BYTES)

// fd: the CSI video device, streaming the preview from USERPTR buffers
// cam_buf[0..count-1] (each slot bytes), all inside arena (arena_len bytes,
// at least FULLRES_BUF_BYTES). The raw frame is captured into the arena, so
// the preview buffers' contents are lost. The preview is streaming again on
// return, also when the capture itself failed; an error return means the
// preview could not be restored.
esp_err_t fullres_capture(int fd, uint8_t *arena, size_t arena_len, uint8_t *const cam_buf[], int count,
                          size_t slot, uint32_t prev_w, uint32_t prev_h);
