// Full-resolution raw still (feasibility spike): stop the preview, switch the
// IMX477 to 4056x3040 RAW10, capture one frame with the ISP bypassed, write it
// to the card as DCIM/RAW_nnnn.RAW, then bring the preview back. Every step is
// timed in the log. The preview is frozen for the whole capture, including the
// card write.
#pragma once
#include <stdint.h>
#include "esp_err.h"

// fd: the CSI video device, streaming the preview. cam_buf: the preview's MMAP
// buffers (count of them), refreshed with the new mappings on return. The
// preview is streaming again on return, also when the capture itself failed;
// an error return means the preview could not be restored.
esp_err_t fullres_capture(int fd, uint8_t *cam_buf[], int count, uint32_t prev_w, uint32_t prev_h);
