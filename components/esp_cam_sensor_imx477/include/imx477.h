/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Sony IMX477 (Raspberry Pi HQ Camera) driver for the ESP32-P4 esp_cam_sensor framework.
 */
#pragma once

#include "esp_cam_sensor.h"
#include "esp_cam_sensor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMX477_SENSOR_NAME   "IMX477"

#ifndef IMX477_SCCB_ADDR
#define IMX477_SCCB_ADDR     0x1a
#endif

#define IMX477_CHIP_ID_VALUE 0x0477
#define IMX477_PID           IMX477_CHIP_ID_VALUE

esp_cam_sensor_device_t *imx477_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif
