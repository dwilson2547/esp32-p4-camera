#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "capture.h"
#include "fullres.h"

static const char *TAG = "fullres";

#define SENSOR_FMT_PREVIEW  0   // imx477 format table index: 1920x1520 binned
#define SENSOR_FMT_FULL     1   // 4056x3040 unbinned
#define FULL_W              4056
#define FULL_H              3040
#define FULL_BYTES          ((size_t)FULL_W * FULL_H * 10 / 8)   // MIPI RAW10: 4 px in 5 bytes

static const int s_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

static bool xioctl(int fd, int req, void *arg, const char *what)
{
    if (ioctl(fd, req, arg) == 0) return true;
    ESP_LOGE(TAG, "%s failed (errno %d)", what, errno);
    return false;
}

static void log_psram(const char *when)
{
    ESP_LOGI(TAG, "%s: PSRAM free %u KB, largest block %u KB", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

static bool log_block(walker_heap_into_t heap, walker_block_info_t blk, void *user)
{
    if (blk.size >= 256 * 1024)
        ESP_LOGI(TAG, "  %s %p %6u KB", blk.used ? "used" : "free", blk.ptr, (unsigned)(blk.size / 1024));
    return true;
}

// Every PSRAM block of 256 KB or more, in address order: shows what splits
// the free space when the full-res buffer does not fit.
static void log_heap_map(const char *when)
{
    log_psram(when);
    heap_caps_walk(MALLOC_CAP_SPIRAM, log_block, NULL);
}

static void stop_stream(int fd)
{
    xioctl(fd, VIDIOC_STREAMOFF, (void *)&s_type, "STREAMOFF");
    struct v4l2_requestbuffers req = { .count = 0, .type = s_type, .memory = V4L2_MEMORY_MMAP };
    xioctl(fd, VIDIOC_REQBUFS, &req, "REQBUFS 0");
}

// Sensor mode, capture format and count MMAP buffers, all queued, streaming.
static bool start_stream(int fd, uint32_t sensor_idx, uint32_t w, uint32_t h, uint32_t fourcc,
                         uint8_t *buf[], uint32_t len[], int count)
{
    struct v4l2_sensor_format_enum e = { .index = sensor_idx };
    if (!xioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &e, "ENUM_SENSOR_FMT")) return false;
    if (!xioctl(fd, VIDIOC_S_SENSOR_FMT, &e.format, "S_SENSOR_FMT")) return false;
    struct v4l2_format f = { .type = s_type };
    f.fmt.pix.width = w;
    f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = fourcc;
    if (!xioctl(fd, VIDIOC_S_FMT, &f, "S_FMT")) return false;
    struct v4l2_requestbuffers req = { .count = count, .type = s_type, .memory = V4L2_MEMORY_MMAP };
    if (!xioctl(fd, VIDIOC_REQBUFS, &req, "REQBUFS")) return false;
    for (int i = 0; i < count; i++) {
        struct v4l2_buffer b = { .type = s_type, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (!xioctl(fd, VIDIOC_QUERYBUF, &b, "QUERYBUF")) return false;
        buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (len) len[i] = b.length;
        if (!xioctl(fd, VIDIOC_QBUF, &b, "QBUF")) return false;
    }
    return xioctl(fd, VIDIOC_STREAMON, (void *)&s_type, "STREAMON");
}

// Quick look at the packed RAW10 data: the high 8 bits of every 4th pixel
// (the first byte of each 5-byte group) on every 16th row, in three bands, so
// a short or garbled frame shows up as a band that differs or is flat.
static void log_stats(const uint8_t *p, size_t used)
{
    const size_t stride = FULL_BYTES / FULL_H;   // 5070
    ESP_LOGI(TAG, "first bytes: %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x",
             p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]);
    static const char *const names[] = { "top", "middle", "bottom" };
    for (int band = 0; band < 3; band++) {
        uint32_t r0 = band * FULL_H / 3, r1 = (band + 1) * FULL_H / 3;
        uint64_t sum = 0;
        uint32_t n = 0, mn = 255, mx = 0, zero = 0;
        for (uint32_t r = r0; r < r1; r += 16) {
            if ((r + 1) * stride > used) break;
            const uint8_t *row = p + r * stride;
            for (size_t x = 0; x < stride; x += 5) {
                uint8_t v = row[x];
                sum += v; n++;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                if (!v) zero++;
            }
        }
        if (n) ESP_LOGI(TAG, "%s third: mean %.1f min %" PRIu32 " max %" PRIu32 " (of 255), %.1f%% zero",
                        names[band], (double)sum / n, mn, mx, 100.0 * zero / n);
        else ESP_LOGW(TAG, "%s third: no data", names[band]);
    }
}

esp_err_t fullres_capture(int fd, uint8_t *cam_buf[], int count, uint32_t prev_w, uint32_t prev_h)
{
    int64_t t0 = esp_timer_get_time();
    stop_stream(fd);
    log_heap_map("preview stopped");
    capture_release_encoder();
    log_heap_map("JPEG buffer freed too");

    uint8_t *raw = NULL;
    uint32_t raw_len = 0;
    int64_t t_on = 0, t_f1 = 0, t_f2 = 0, t_saved = 0;
    bool ok = start_stream(fd, SENSOR_FMT_FULL, FULL_W, FULL_H, V4L2_PIX_FMT_SBGGR10, &raw, &raw_len, 1);
    t_on = esp_timer_get_time();
    if (ok) {
        // The first frame after stream-on is thrown away (it can be partly
        // exposed with the old settings); the second one is kept.
        struct v4l2_buffer b = { .type = s_type, .memory = V4L2_MEMORY_MMAP };
        ok = xioctl(fd, VIDIOC_DQBUF, &b, "DQBUF 1");
        t_f1 = esp_timer_get_time();
        if (ok) {
            ESP_LOGI(TAG, "frame 1: %" PRIu32 " bytes", b.bytesused);
            ok = xioctl(fd, VIDIOC_QBUF, &b, "QBUF") && xioctl(fd, VIDIOC_DQBUF, &b, "DQBUF 2");
            t_f2 = esp_timer_get_time();
        }
        xioctl(fd, VIDIOC_STREAMOFF, (void *)&s_type, "STREAMOFF");
        if (ok) {
            ESP_LOGI(TAG, "frame 2: %" PRIu32 " bytes (buffer %" PRIu32 ", expected %u)",
                     b.bytesused, raw_len, (unsigned)FULL_BYTES);
            size_t used = b.bytesused && b.bytesused < FULL_BYTES ? b.bytesused : FULL_BYTES;
            if (raw_len < used) used = raw_len;
            log_stats(raw, used);
            ok = capture_save_raw(raw, used) == ESP_OK;
            t_saved = esp_timer_get_time();
        }
    }
    struct v4l2_requestbuffers req = { .count = 0, .type = s_type, .memory = V4L2_MEMORY_MMAP };
    xioctl(fd, VIDIOC_REQBUFS, &req, "REQBUFS 0");

    int64_t t_back0 = esp_timer_get_time();
    bool back = start_stream(fd, SENSOR_FMT_PREVIEW, prev_w, prev_h, V4L2_PIX_FMT_RGB565, cam_buf, NULL, count);
    int64_t t_back = esp_timer_get_time();
    if (back) capture_restore_encoder();
    if (t_f2) {
        ESP_LOGI(TAG, "timings: stop preview + mode switch + stream on %.0f ms, first frame %.0f ms, "
                 "second frame %.0f ms later, card write %.0f ms, back to preview %.0f ms; total %.1f s",
                 (t_on - t0) / 1e3, (t_f1 - t_on) / 1e3, (t_f2 - t_f1) / 1e3,
                 t_saved ? (t_saved - t_f2) / 1e3 : 0.0, (t_back - t_back0) / 1e3, (t_back - t0) / 1e6);
    }
    ESP_LOGI(TAG, "full-res capture %s; preview %s", ok ? "done" : "FAILED", back ? "restored" : "NOT restored");
    log_psram("preview back");
    return back ? ESP_OK : ESP_FAIL;
}
