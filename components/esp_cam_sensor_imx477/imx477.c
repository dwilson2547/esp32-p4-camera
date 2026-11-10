/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Sony IMX477 (Raspberry Pi HQ Camera) driver for the ESP32-P4 esp_cam_sensor
 * framework.
 *
 * Structure mirrors the IMX219 driver in esp_cam_sensor_imx (Apache-2.0). The
 * register sequences and timing come from the Raspberry Pi Linux driver
 * drivers/media/i2c/imx477.c (GPL-2.0), which is why this file is GPL-2.0.
 *
 * Two modes, both RAW10:
 *   1920x1520: the Linux 2x2-binned full-field mode digitally cropped from
 *              2028 to 1920 wide to fit the P4's ISP (the preview).
 *   4056x3040: full resolution, unbinned. Wider than the ISP can process, so
 *              it is only captured raw with the ISP bypassed.
 */
#include <string.h>
#include "sdkconfig.h"
#include <inttypes.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "imx477_regs.h"
#include "imx477_settings.h"
#include "imx477.h"

#define delay_ms(ms) vTaskDelay((ms > portTICK_PERIOD_MS ? ms / portTICK_PERIOD_MS : 1))

/*
 * Clocking, as Linux sets it for its default 450 MHz link:
 *   INCK 24 MHz (the HQ board's own oscillator)
 *   IOP PLL multiplier = link * 2 * SYSCK_DIV(2) * PREDIV(2) / INCK = 150
 *   2 lanes x 900 Mbps; requested link bit rate 1800 Mbps (16.16 fixed point)
 *   pixel rate fixed at 840 MHz
 * Line length is Linux's minimum for a 2028-wide 10-bit line
 * (2028 * 10 * 840 / 1800 + 500 = 9964). Kept at the 2028 value rather than
 * recomputed for 1920, because the sensor still reads out the full binned line
 * before the digital crop. Frame length is Linux's default for the mode.
 */
#define IMX477_XCLK_FREQ_HZ     24000000
#define IMX477_IOP_MPY          150
#define IMX477_LINK_BIT_RATE_16P16 (1800u << 16)
#define IMX477_MIPI_LANE_RATE   900000000
#define IMX477_PIXEL_RATE       840000000
#define IMX477_HTS              9964
#define IMX477_VTS              2197
#define IMX477_TLINE_NS         11862   /* HTS / pixel rate */
#define IMX477_FPS              38      /* 840e6 / (9964 * 2197) = 38.4 */
/*
 * Full resolution: Linux minimum line length for 4056 x 10 bit
 * (4056 * 10 * 840 / 1800 + 500 = 19428) and its default frame length 3500.
 */
#define IMX477_FULL_HTS         19428
#define IMX477_FULL_VTS         3500
#define IMX477_FULL_TLINE_NS    23129   /* 19428 / 840 MHz */
#define IMX477_FULL_FPS         12      /* 840e6 / (19428 * 3500) = 12.35 */

static const char *TAG = "imx477";

enum {
    IMX477_FMT_1920x1520_RAW10 = 0,
    IMX477_FMT_4056x3040_RAW10 = 1,
};

static const esp_cam_sensor_isp_info_t imx477_isp_info[] = {
    [IMX477_FMT_1920x1520_RAW10] = {
        .isp_v1_info = {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .pclk = IMX477_PIXEL_RATE,
            .hts = IMX477_HTS,
            .vts = IMX477_VTS,
            .exp_def = IMX477_EXPOSURE_DEFAULT,
            .gain_def = IMX477_ANA_GAIN_DEFAULT,
            .tline_ns = IMX477_TLINE_NS,
            .bayer_type = ESP_CAM_SENSOR_BAYER_RGGB,
        }
    },
    [IMX477_FMT_4056x3040_RAW10] = {
        .isp_v1_info = {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .pclk = IMX477_PIXEL_RATE,
            .hts = IMX477_FULL_HTS,
            .vts = IMX477_FULL_VTS,
            .exp_def = IMX477_EXPOSURE_DEFAULT,
            .gain_def = IMX477_ANA_GAIN_DEFAULT,
            .tline_ns = IMX477_FULL_TLINE_NS,
            .bayer_type = ESP_CAM_SENSOR_BAYER_RGGB,
        }
    },
};

static const esp_cam_sensor_format_t imx477_format_info[] = {
    [IMX477_FMT_1920x1520_RAW10] = {
        .name = "MIPI_2lane_24Minput_RAW10_1920x1520_38fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = IMX477_XCLK_FREQ_HZ,
        .width = 1920,
        .height = 1520,
        .regs = imx477_mode_1920x1520_regs,
        .regs_size = ARRAY_SIZE(imx477_mode_1920x1520_regs),
        .fps = IMX477_FPS,
        .isp_info = &imx477_isp_info[IMX477_FMT_1920x1520_RAW10],
        .mipi_info = {
            .mipi_clk = IMX477_MIPI_LANE_RATE,
            .lane_num = 2,
            .line_sync_en = 0,
        },
        .reserved = NULL,
    },
    [IMX477_FMT_4056x3040_RAW10] = {
        .name = "MIPI_2lane_24Minput_RAW10_4056x3040_12fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW10,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = IMX477_XCLK_FREQ_HZ,
        .width = 4056,
        .height = 3040,
        .regs = imx477_mode_4056x3040_regs,
        .regs_size = ARRAY_SIZE(imx477_mode_4056x3040_regs),
        .fps = IMX477_FULL_FPS,
        .isp_info = &imx477_isp_info[IMX477_FMT_4056x3040_RAW10],
        .mipi_info = {
            .mipi_clk = IMX477_MIPI_LANE_RATE,
            .lane_num = 2,
            .line_sync_en = 0,
        },
        .reserved = NULL,
    },
};

#define IMX477_DEFAULT_FORMAT_INDEX IMX477_FMT_1920x1520_RAW10

/* Index of 2.00x in imx477_total_gain_val_map. */
#define IMX477_DEFAULT_GAIN_INDEX 12

/*
 * Bayer phase by flip state, indexed (vflip << 1) | hmirror. Same order as the
 * Linux driver's media-bus code table. The unflipped phase is RGGB because the
 * readout window starts at (0,0) and the digital crop's X offset (54) is even.
 */
static const esp_cam_sensor_bayer_pattern_t imx477_bayer_by_flip[4] = {
    [0] = ESP_CAM_SENSOR_BAYER_RGGB,
    [1] = ESP_CAM_SENSOR_BAYER_GRBG,
    [2] = ESP_CAM_SENSOR_BAYER_GBRG,
    [3] = ESP_CAM_SENSOR_BAYER_BGGR,
};

/* Per-device state; format/isp_info shadow the table so bayer_type can follow flips. */
typedef struct {
    uint32_t exposure_val;
    uint32_t gain_index;
    uint8_t  hmirror;
    uint8_t  vflip;
    esp_cam_sensor_format_t   format;
    esp_cam_sensor_isp_info_t isp_info;
} imx477_para_t;

/* ------------------------------------------------------------------ */
/* SCCB helpers (16-bit register address, 8-bit value)                 */
/* ------------------------------------------------------------------ */
static esp_err_t imx477_read(esp_sccb_io_handle_t sccb, uint16_t reg, uint8_t *val)
{
    return esp_sccb_transmit_receive_reg_a16v8(sccb, reg, val);
}

static esp_err_t imx477_write(esp_sccb_io_handle_t sccb, uint16_t reg, uint8_t val)
{
    return esp_sccb_transmit_reg_a16v8(sccb, reg, val);
}

static esp_err_t imx477_write16(esp_sccb_io_handle_t sccb, uint16_t reg, uint16_t val)
{
    esp_err_t ret = imx477_write(sccb, reg, (val >> 8) & 0xff);
    if (ret == ESP_OK) {
        ret = imx477_write(sccb, reg + 1, val & 0xff);
    }
    return ret;
}

static esp_err_t imx477_write32(esp_sccb_io_handle_t sccb, uint16_t reg, uint32_t val)
{
    esp_err_t ret = imx477_write16(sccb, reg, (val >> 16) & 0xffff);
    if (ret == ESP_OK) {
        ret = imx477_write16(sccb, reg + 2, val & 0xffff);
    }
    return ret;
}

static esp_err_t imx477_write_array(esp_sccb_io_handle_t sccb, const imx477_reginfo_t *regs)
{
    esp_err_t ret = ESP_OK;
    int i = 0;
    while (ret == ESP_OK && regs[i].reg != IMX477_REG_END) {
        if (regs[i].reg == IMX477_REG_DELAY) {
            delay_ms(regs[i].val);
        } else {
            ret = imx477_write(sccb, regs[i].reg, regs[i].val);
        }
        i++;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write failed at entry %d (reg 0x%04x)", i - 1, regs[i - 1].reg);
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Sensor operations                                                   */
/* ------------------------------------------------------------------ */
static esp_err_t imx477_get_sensor_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id)
{
    uint8_t h = 0, l = 0;
    esp_err_t ret = imx477_read(dev->sccb_handle, IMX477_REG_CHIP_ID_H, &h);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "read chip id high failed");
    ret = imx477_read(dev->sccb_handle, IMX477_REG_CHIP_ID_L, &l);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "read chip id low failed");
    id->pid = (h << 8) | l;
    return ESP_OK;
}

static esp_err_t imx477_set_stream(esp_cam_sensor_device_t *dev, int enable)
{
    esp_err_t ret = imx477_write(dev->sccb_handle, IMX477_REG_MODE_SELECT, enable ? 0x01 : 0x00);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "set stream failed");
    dev->stream_status = enable;
    ESP_LOGI(TAG, "stream %s", enable ? "on" : "off");
    return ret;
}

static esp_err_t imx477_apply_orientation(esp_cam_sensor_device_t *dev)
{
    imx477_para_t *para = (imx477_para_t *)dev->priv;
    if (para == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t regval = (para->hmirror ? IMX477_ORIENTATION_HMIRROR : 0) |
                     (para->vflip ? IMX477_ORIENTATION_VFLIP : 0);
    esp_err_t ret = imx477_write(dev->sccb_handle, IMX477_REG_ORIENTATION, regval);
    if (ret == ESP_OK) {
        para->isp_info.isp_v1_info.bayer_type =
            imx477_bayer_by_flip[(para->vflip ? 2 : 0) | (para->hmirror ? 1 : 0)];
    }
    return ret;
}

static esp_err_t imx477_set_orientation(esp_cam_sensor_device_t *dev, int hmirror, int vflip)
{
    imx477_para_t *para = (imx477_para_t *)dev->priv;
    if (para == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    para->hmirror = hmirror ? 1 : 0;
    para->vflip = vflip ? 1 : 0;
    if (dev->stream_status) {
        ESP_LOGW(TAG, "flip changed while streaming: the ISP keeps the Bayer phase it latched - "
                 "re-set the sensor format to pick it up");
    }
    return imx477_apply_orientation(dev);
}

/* Line time of the current mode, for exposure conversions. */
static uint32_t imx477_tline_ns(esp_cam_sensor_device_t *dev)
{
    if (dev->cur_format && dev->cur_format->isp_info && dev->cur_format->isp_info->isp_v1_info.tline_ns) {
        return dev->cur_format->isp_info->isp_v1_info.tline_ns;
    }
    return IMX477_TLINE_NS;
}

static uint32_t imx477_exposure_max(esp_cam_sensor_device_t *dev)
{
    uint32_t max = IMX477_EXPOSURE_MAX;
    if (dev->cur_format && dev->cur_format->isp_info) {
        uint32_t vts = dev->cur_format->isp_info->isp_v1_info.vts;
        if (vts > IMX477_EXPOSURE_OFFSET + IMX477_EXPOSURE_MIN) {
            max = vts - IMX477_EXPOSURE_OFFSET;
        }
    }
    return max > IMX477_EXPOSURE_MAX ? IMX477_EXPOSURE_MAX : max;
}

static esp_err_t imx477_set_exposure(esp_cam_sensor_device_t *dev, uint32_t lines)
{
    uint32_t max = imx477_exposure_max(dev);
    if (lines < IMX477_EXPOSURE_MIN) {
        lines = IMX477_EXPOSURE_MIN;
    }
    if (lines > max) {
        lines = max;
    }
    esp_err_t ret = imx477_write16(dev->sccb_handle, IMX477_REG_EXPOSURE_H, (uint16_t)lines);
    if (ret == ESP_OK && dev->priv) {
        imx477_para_t *para = (imx477_para_t *)dev->priv;
        if (para->exposure_val != lines) {
            ESP_LOGI(TAG, "exposure %" PRIu32 " lines", lines);
        }
        para->exposure_val = lines;
    }
    return ret;
}

/* Analog gain: 10-bit code, gain = 1024 / (1024 - code). */
static esp_err_t imx477_set_analog_gain(esp_cam_sensor_device_t *dev, uint32_t code)
{
    if (code > IMX477_ANA_GAIN_MAX) {
        code = IMX477_ANA_GAIN_MAX;
    }
    return imx477_write16(dev->sccb_handle, IMX477_REG_ANALOG_GAIN_H, (uint16_t)code);
}

static esp_err_t imx477_set_gain_index(esp_cam_sensor_device_t *dev, uint32_t index)
{
    if (index >= ARRAY_SIZE(imx477_ana_gain_code_map)) {
        index = ARRAY_SIZE(imx477_ana_gain_code_map) - 1;
    }
    esp_err_t ret = imx477_write16(dev->sccb_handle, IMX477_REG_ANALOG_GAIN_H,
                                   imx477_ana_gain_code_map[index]);
    if (ret == ESP_OK && dev->priv) {
        imx477_para_t *para = (imx477_para_t *)dev->priv;
        if (para->gain_index != index) {
            ESP_LOGI(TAG, "gain index %" PRIu32 " (%" PRIu32 " mx, code %u)", index,
                     imx477_total_gain_val_map[index], imx477_ana_gain_code_map[index]);
        }
        para->gain_index = index;
    }
    return ret;
}

static esp_err_t imx477_set_digital_gain(esp_cam_sensor_device_t *dev, uint32_t val)
{
    if (val < IMX477_DGTL_GAIN_MIN) {
        val = IMX477_DGTL_GAIN_MIN;
    }
    if (val > IMX477_DGTL_GAIN_MAX) {
        val = IMX477_DGTL_GAIN_MAX;
    }
    return imx477_write16(dev->sccb_handle, IMX477_REG_DIGITAL_GAIN_H, (uint16_t)val);
}

/*
 * Test pattern, by V4L2_CID_TEST_PATTERN value:
 *   0 off, 1 colour bars, 2 solid red, 3 solid green, 4 solid blue, 5 solid white.
 * Solid colours use the sensor's solid-colour mode (0x0600 = 1) with the
 * per-channel values in 0x0602 (R), 0x0604 (Gr), 0x0606 (B), 0x0608 (Gb), 12-bit.
 */
static esp_err_t imx477_set_test_pattern(esp_cam_sensor_device_t *dev, int mode)
{
    static const char *const names[] = { "off", "colour bars", "solid red", "solid green", "solid blue", "solid white" };
    static const uint16_t rgb[][4] = {   /* R, Gr, B, Gb */
        [2] = { 0x0fff, 0, 0, 0 }, [3] = { 0, 0x0fff, 0, 0x0fff },
        [4] = { 0, 0, 0x0fff, 0 }, [5] = { 0x0fff, 0x0fff, 0x0fff, 0x0fff },
    };
    if (mode < 0 || mode > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_sccb_io_handle_t s = dev->sccb_handle;
    esp_err_t ret = ESP_OK;
    if (mode >= 2) {
        ret = imx477_write16(s, 0x0602, rgb[mode][0]);
        if (ret == ESP_OK) ret = imx477_write16(s, 0x0604, rgb[mode][1]);
        if (ret == ESP_OK) ret = imx477_write16(s, 0x0606, rgb[mode][2]);
        if (ret == ESP_OK) ret = imx477_write16(s, 0x0608, rgb[mode][3]);
    }
    if (ret == ESP_OK) {
        ret = imx477_write16(s, IMX477_REG_TEST_PATTERN_H,
                             mode == 0 ? IMX477_TEST_PATTERN_DISABLE :
                             mode == 1 ? IMX477_TEST_PATTERN_COLORBARS : 1 /* solid colour */);
    }
    ESP_LOGI(TAG, "test pattern: %s", names[mode]);
    return ret;
}

static esp_err_t imx477_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
{
    esp_err_t ret = ESP_OK;
    switch (qdesc->id) {
    case ESP_CAM_SENSOR_VFLIP:
    case ESP_CAM_SENSOR_HMIRROR:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 0;
        qdesc->number.maximum = 1;
        qdesc->number.step = 1;
        qdesc->default_value = 0;
        break;
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = IMX477_EXPOSURE_MIN;
        qdesc->number.maximum = imx477_exposure_max(dev);
        qdesc->number.step = 1;
        qdesc->default_value = IMX477_EXPOSURE_DEFAULT;
        break;
    case ESP_CAM_SENSOR_GAIN:
        /* Menu control (see the IMX219 driver): elements are total gain in
           milli-units; the value set later is an index into this table. */
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION;
        qdesc->enumeration.count = ARRAY_SIZE(imx477_total_gain_val_map);
        qdesc->enumeration.elements = imx477_total_gain_val_map;
        qdesc->default_value = IMX477_DEFAULT_GAIN_INDEX;
        break;
    case ESP_CAM_SENSOR_ANGAIN:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = IMX477_ANA_GAIN_MIN;
        qdesc->number.maximum = IMX477_ANA_GAIN_MAX;
        qdesc->number.step = 1;
        qdesc->default_value = IMX477_ANA_GAIN_DEFAULT;
        break;
    case ESP_CAM_SENSOR_DGAIN:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = IMX477_DGTL_GAIN_MIN;
        qdesc->number.maximum = IMX477_DGTL_GAIN_MAX;
        qdesc->number.step = 1;
        qdesc->default_value = IMX477_DGTL_GAIN_DEFAULT;
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

static esp_err_t imx477_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
{
    imx477_para_t *para = (imx477_para_t *)dev->priv;
    if (para == NULL || arg == NULL || size < sizeof(uint32_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        *(uint32_t *)arg = para->exposure_val;
        break;
    case ESP_CAM_SENSOR_GAIN:
        *(uint32_t *)arg = para->gain_index;
        break;
    case ESP_CAM_SENSOR_HMIRROR:
        *(uint32_t *)arg = para->hmirror;
        break;
    case ESP_CAM_SENSOR_VFLIP:
        *(uint32_t *)arg = para->vflip;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t imx477_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
{
    const imx477_para_t *para = (const imx477_para_t *)dev->priv;
    esp_err_t ret = ESP_OK;
    switch (id) {
    case ESP_CAM_SENSOR_VFLIP:
        ret = imx477_set_orientation(dev, para ? para->hmirror : 0, *(const int *)arg);
        break;
    case ESP_CAM_SENSOR_HMIRROR:
        ret = imx477_set_orientation(dev, *(const int *)arg, para ? para->vflip : 0);
        break;
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        ret = imx477_set_exposure(dev, *(const uint32_t *)arg);
        break;
    case ESP_CAM_SENSOR_EXPOSURE_US: {
        uint32_t us = *(const uint32_t *)arg;
        ret = imx477_set_exposure(dev, (uint32_t)(((uint64_t)us * 1000) / imx477_tline_ns(dev)));
        break;
    }
    case ESP_CAM_SENSOR_GAIN:
        ret = imx477_set_gain_index(dev, *(const uint32_t *)arg);
        break;
    case ESP_CAM_SENSOR_ANGAIN:
        ret = imx477_set_analog_gain(dev, *(const uint32_t *)arg);
        break;
    case ESP_CAM_SENSOR_GROUP_EXP_GAIN: {
        const esp_cam_sensor_gh_exp_gain_t *g = (const esp_cam_sensor_gh_exp_gain_t *)arg;
        uint32_t lines;
        if (g->exposure_val != 0) {
            lines = g->exposure_val;
        } else if (g->exposure_us != 0) {
            lines = (uint32_t)(((uint64_t)g->exposure_us * 1000) / imx477_tline_ns(dev));
        } else {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        ret = imx477_set_exposure(dev, lines);
        if (ret == ESP_OK) {
            ret = imx477_set_gain_index(dev, g->gain_index);
        }
        break;
    }
    case ESP_CAM_SENSOR_DGAIN:
        ret = imx477_set_digital_gain(dev, *(const uint32_t *)arg);
        break;
    default:
        ESP_LOGE(TAG, "set id=%" PRIx32 " not supported", id);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

static esp_err_t imx477_query_support_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, formats);
    formats->count = ARRAY_SIZE(imx477_format_info);
    formats->format_array = &imx477_format_info[0];
    return ESP_OK;
}

static esp_err_t imx477_query_support_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *caps)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, caps);
    caps->fmt_raw = 1;
    return ESP_OK;
}

static esp_err_t imx477_select_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    imx477_para_t *para = (imx477_para_t *)dev->priv;
    if (para == NULL) {
        dev->cur_format = format;
        return ESP_ERR_INVALID_STATE;
    }
    para->format = *format;
    if (format->isp_info) {
        /* memcpy: isp_v1_info.version is const, so the union isn't assignable;
           skipped when re-selecting the shadow itself (no overlapping copy). */
        if (format->isp_info != &para->isp_info) {
            memcpy(&para->isp_info, format->isp_info, sizeof(para->isp_info));
        }
        para->format.isp_info = &para->isp_info;
    }
    dev->cur_format = &para->format;
    return ESP_OK;
}

/* Everything Linux imx477_start_streaming() writes, minus the final stream-on. */
static esp_err_t imx477_write_mode(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    esp_sccb_io_handle_t s = dev->sccb_handle;
    esp_err_t ret = imx477_write(s, IMX477_REG_MODE_SELECT, 0x00);

    if (ret == ESP_OK) ret = imx477_write_array(s, imx477_common_regs);
    if (ret == ESP_OK) ret = imx477_write16(s, IMX477_REG_IOP_MPY_H, IMX477_IOP_MPY);
    if (ret == ESP_OK) ret = imx477_write32(s, IMX477_REG_REQ_LINK_BIT_RATE, IMX477_LINK_BIT_RATE_16P16);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "common settings failed");

    ret = imx477_write_array(s, (const imx477_reginfo_t *)format->regs);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "mode regs failed");

    /* RAW10: CSI data type, pixel clock divider and AD bit mode follow bpp. */
    ret = imx477_write(s, IMX477_REG_CSI_DT_FMT_H, 10);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_CSI_DT_FMT_L, 10);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_IOP_PXCK_DIV, 10);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_ADBIT_MODE, 0);
    /* On-sensor defect pixel correction on (Linux default). */
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_DPC_1, 1);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_DPC_2, 1);
    /* Standalone sync (Linux trigger_mode 0). */
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_EXTOUT_EN, 0);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_MS_SEL, 1);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_XVS_IO_CTRL, 0);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_MC_MODE, 0);
    /* Frame timing (Linux sets these through the HBLANK/VBLANK controls). */
    const esp_cam_sensor_isp_info_t *ii = format->isp_info;
    if (ret == ESP_OK) ret = imx477_write16(s, IMX477_REG_LINE_LENGTH_H, ii ? ii->isp_v1_info.hts : IMX477_HTS);
    if (ret == ESP_OK) ret = imx477_write16(s, IMX477_REG_FRAME_LENGTH_H, ii ? ii->isp_v1_info.vts : IMX477_VTS);
    if (ret == ESP_OK) ret = imx477_write(s, IMX477_REG_LONG_EXP_SHIFT, 0);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "format/timing regs failed");
    return ESP_OK;
}

static esp_err_t imx477_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    if (format == NULL) {
        format = &imx477_format_info[IMX477_DEFAULT_FORMAT_INDEX];
    }

    /*
     * A mode change keeps the exposure time and gain the auto exposure had
     * reached, so a full-resolution still is exposed like the preview. The
     * line time differs between modes, so the exposure is carried in ns.
     * Nothing to carry on the first set_format (exposure_val is still 0).
     */
    imx477_para_t *pp = (imx477_para_t *)dev->priv;
    const uint64_t carry_ns = pp ? (uint64_t)pp->exposure_val * imx477_tline_ns(dev) : 0;
    const uint32_t carry_gain = pp ? pp->gain_index : IMX477_DEFAULT_GAIN_INDEX;
    esp_err_t ret = imx477_write_mode(dev, format);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = imx477_select_format(dev, format);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "no device state to shadow the format into");
    ret = imx477_apply_orientation(dev);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "apply orientation failed");
    {
        static const char *const bayer[] = { "RGGB", "GRBG", "GBRG", "BGGR" };
        const imx477_para_t *para = (const imx477_para_t *)dev->priv;
        ESP_LOGI(TAG, "orientation: hmirror %d vflip %d, bayer %s", para->hmirror, para->vflip,
                 bayer[(para->vflip ? 2 : 0) | (para->hmirror ? 1 : 0)]);
    }

    if (carry_ns) {
        imx477_set_exposure(dev, (uint32_t)(carry_ns / imx477_tline_ns(dev)));
        imx477_set_gain_index(dev, carry_gain);
    } else {
        imx477_set_exposure(dev, IMX477_EXPOSURE_DEFAULT);
        imx477_set_gain_index(dev, IMX477_DEFAULT_GAIN_INDEX);
    }
    imx477_set_digital_gain(dev, IMX477_DGTL_GAIN_DEFAULT);

#if CONFIG_CAMERA_IMX477_TEST_PATTERN_AT_START
    imx477_set_test_pattern(dev, 1);
#endif

    ESP_LOGI(TAG, "set format: %s", format->name);
    return ESP_OK;
}

static esp_err_t imx477_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, format);
    if (dev->cur_format == NULL) {
        return ESP_FAIL;
    }
    memcpy(format, dev->cur_format, sizeof(esp_cam_sensor_format_t));
    return ESP_OK;
}

static esp_err_t imx477_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    esp_err_t ret = ESP_OK;
    uint8_t regval = 0;
    esp_cam_sensor_reg_val_t *sensor_reg;

    switch (cmd) {
    case ESP_CAM_SENSOR_IOC_HW_RESET:
        if (dev->reset_pin >= 0) {
            gpio_set_level(dev->reset_pin, 0);
            delay_ms(10);
            gpio_set_level(dev->reset_pin, 1);
            delay_ms(10);
        }
        break;
    case ESP_CAM_SENSOR_IOC_SW_RESET:
        ret = imx477_write(dev->sccb_handle, 0x0103, 0x01);
        delay_ms(5);
        break;
    case ESP_CAM_SENSOR_IOC_S_STREAM:
        ret = imx477_set_stream(dev, *(int *)arg);
        break;
    case ESP_CAM_SENSOR_IOC_S_TEST_PATTERN:
        ret = imx477_set_test_pattern(dev, *(int *)arg);
        break;
    case ESP_CAM_SENSOR_IOC_S_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = imx477_write(dev->sccb_handle, sensor_reg->regaddr, sensor_reg->value);
        break;
    case ESP_CAM_SENSOR_IOC_G_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = imx477_read(dev->sccb_handle, sensor_reg->regaddr, &regval);
        if (ret == ESP_OK) {
            sensor_reg->value = regval;
        }
        break;
    case ESP_CAM_SENSOR_IOC_G_CHIP_ID:
        ret = imx477_get_sensor_id(dev, (esp_cam_sensor_id_t *)arg);
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

static esp_err_t imx477_power_on(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ESP_OK;
    if (dev->pwdn_pin >= 0) {
        gpio_config_t conf = { .pin_bit_mask = 1ULL << dev->pwdn_pin, .mode = GPIO_MODE_OUTPUT };
        ret = gpio_config(&conf);
        ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "pwdn pin config failed");
        gpio_set_level(dev->pwdn_pin, 1);
        delay_ms(10);
    }
    if (dev->reset_pin >= 0) {
        gpio_config_t conf = { .pin_bit_mask = 1ULL << dev->reset_pin, .mode = GPIO_MODE_OUTPUT };
        ret = gpio_config(&conf);
        ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "reset pin config failed");
        gpio_set_level(dev->reset_pin, 0);
        delay_ms(10);
        gpio_set_level(dev->reset_pin, 1);
        delay_ms(10);
    }
    return ret;
}

static esp_err_t imx477_power_off(esp_cam_sensor_device_t *dev)
{
    if (dev->reset_pin >= 0) {
        gpio_set_level(dev->reset_pin, 0);
    }
    if (dev->pwdn_pin >= 0) {
        gpio_set_level(dev->pwdn_pin, 0);
    }
    return ESP_OK;
}

static esp_err_t imx477_delete(esp_cam_sensor_device_t *dev)
{
    free(dev);
    return ESP_OK;
}

static const esp_cam_sensor_ops_t imx477_ops = {
    .query_para_desc = imx477_query_para_desc,
    .get_para_value = imx477_get_para_value,
    .set_para_value = imx477_set_para_value,
    .query_support_formats = imx477_query_support_formats,
    .query_support_capability = imx477_query_support_capability,
    .set_format = imx477_set_format,
    .get_format = imx477_get_format,
    .priv_ioctl = imx477_priv_ioctl,
    .del = imx477_delete,
};

esp_cam_sensor_device_t *imx477_detect(esp_cam_sensor_config_t *config)
{
    if (config == NULL) {
        return NULL;
    }
    esp_cam_sensor_device_t *dev = calloc(1, sizeof(esp_cam_sensor_device_t) + sizeof(imx477_para_t));
    if (dev == NULL) {
        ESP_LOGE(TAG, "no memory for camera device");
        return NULL;
    }
    dev->priv = (uint8_t *)dev + sizeof(esp_cam_sensor_device_t);
    dev->name = (char *)IMX477_SENSOR_NAME;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->sensor_port = config->sensor_port;
    dev->ops = &imx477_ops;
    imx477_select_format(dev, &imx477_format_info[IMX477_DEFAULT_FORMAT_INDEX]);
#ifdef CONFIG_CAMERA_IMX477_HMIRROR
    ((imx477_para_t *)dev->priv)->hmirror = 1;
#endif
#ifdef CONFIG_CAMERA_IMX477_VFLIP
    ((imx477_para_t *)dev->priv)->vflip = 1;
#endif

    if (config->sensor_port != ESP_CAM_SENSOR_MIPI_CSI) {
        ESP_LOGE(TAG, "only MIPI-CSI is supported");
        goto err;
    }
    if (imx477_power_on(dev) != ESP_OK) {
        ESP_LOGE(TAG, "power on failed");
        goto err;
    }
    if (imx477_get_sensor_id(dev, &dev->id) != ESP_OK) {
        ESP_LOGE(TAG, "get chip id failed");
        goto err;
    }
    if (dev->id.pid != IMX477_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "sensor is not IMX477, PID=0x%04x", dev->id.pid);
        goto err;
    }
    ESP_LOGI(TAG, "detected IMX477, PID=0x%04x", dev->id.pid);
    return dev;

err:
    imx477_power_off(dev);
    free(dev);
    return NULL;
}

#if CONFIG_CAMERA_IMX477_AUTO_DETECT_MIPI_INTERFACE_SENSOR
ESP_CAM_SENSOR_DETECT_FN(imx477_detect, ESP_CAM_SENSOR_MIPI_CSI, IMX477_SCCB_ADDR)
{
    ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    return imx477_detect(config);
}
#endif
