/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * IMX477 register addresses and limits, as used by the Raspberry Pi Linux
 * driver drivers/media/i2c/imx477.c (rpi-6.12.y).
 */
#pragma once

#define IMX477_REG_CHIP_ID_H        0x0016
#define IMX477_REG_CHIP_ID_L        0x0017
#define IMX477_REG_MODE_SELECT      0x0100
#define IMX477_REG_ORIENTATION      0x0101
#define  IMX477_ORIENTATION_HMIRROR 0x01
#define  IMX477_ORIENTATION_VFLIP   0x02
#define IMX477_REG_CSI_DT_FMT_H     0x0112
#define IMX477_REG_CSI_DT_FMT_L     0x0113
#define IMX477_REG_EXPOSURE_H       0x0202
#define IMX477_REG_ANALOG_GAIN_H    0x0204
#define IMX477_REG_DIGITAL_GAIN_H   0x020e
#define IMX477_REG_IOP_PXCK_DIV     0x0309
#define IMX477_REG_IOP_MPY_H        0x030e
#define IMX477_REG_FRAME_LENGTH_H   0x0340
#define IMX477_REG_LINE_LENGTH_H    0x0342
#define IMX477_REG_TEST_PATTERN_H   0x0600
#define IMX477_REG_REQ_LINK_BIT_RATE 0x0820   /* 32-bit, 16.16 Mbps */
#define IMX477_REG_DPC_1            0x0b05
#define IMX477_REG_DPC_2            0x0b06
#define IMX477_REG_LONG_EXP_SHIFT   0x3100
#define IMX477_REG_XVS_IO_CTRL      0x3040
#define IMX477_REG_MS_SEL           0x3041
#define IMX477_REG_MC_MODE          0x3f0b
#define IMX477_REG_ADBIT_MODE       0x3f0d
#define IMX477_REG_EXTOUT_EN        0x4b81

#define IMX477_TEST_PATTERN_DISABLE    0
#define IMX477_TEST_PATTERN_COLORBARS  2

#define IMX477_EXPOSURE_OFFSET      22
#define IMX477_EXPOSURE_MIN         4
#define IMX477_EXPOSURE_MAX         (0xffdc - IMX477_EXPOSURE_OFFSET)
#define IMX477_EXPOSURE_DEFAULT     0x640

#define IMX477_ANA_GAIN_MIN         0
#define IMX477_ANA_GAIN_MAX         978
#define IMX477_ANA_GAIN_DEFAULT     0

#define IMX477_DGTL_GAIN_MIN        0x0100
#define IMX477_DGTL_GAIN_MAX        0xffff
#define IMX477_DGTL_GAIN_DEFAULT    0x0100
