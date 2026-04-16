/*
 * ESP32-S3 eFuse emulation
 *
 * Copyright (c) 2023-2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#pragma once

#include "esp_efuse.h"

#define TYPE_ESP32S3_EFUSE "nvram.esp32s3.efuse"
#define ESP32S3_EFUSE(obj) OBJECT_CHECK(ESP32S3EfuseState, (obj), TYPE_ESP32S3_EFUSE)
#define ESP32S3_EFUSE_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32S3EfuseClass, obj, TYPE_ESP32S3_EFUSE)
#define ESP32S3_EFUSE_CLASS(klass) OBJECT_CLASS_CHECK(ESP32S3EfuseClass, klass, TYPE_ESP32S3_EFUSE)


typedef struct ESP32S3EfuseState {
    ESPEfuseState parent;
} ESP32S3EfuseState;


typedef struct ESP32S3EfuseClass {
    ESPEfuseClass parent_class;
    DeviceRealize parent_realize;
} ESP32S3EfuseClass;

#define ESP32S3_EFUSE_CLK_RESET        0x00000002
#define ESP32S3_EFUSE_CLK_WR_MASK      0x00010007
#define ESP32S3_EFUSE_CONF_WR_MASK     0x0000ffff
#define ESP32S3_EFUSE_DAC_CONF_RESET   0x0001fe1c
#define ESP32S3_EFUSE_DAC_CONF_WR_MASK 0x0003ffff
#define ESP32S3_EFUSE_RD_TIM_RESET     0x12000000
#define ESP32S3_EFUSE_RD_TIM_WR_MASK   0xff000000
#define ESP32S3_EFUSE_WR_TIM1_RESET    0x00288000
#define ESP32S3_EFUSE_WR_TIM1_WR_MASK  0x00ffff00
#define ESP32S3_EFUSE_WR_TIM2_RESET    0x00000190
#define ESP32S3_EFUSE_WR_TIM2_WR_MASK  0x0000ffff
#define ESP32S3_EFUSE_DATE_RESET       0x02101280
#define ESP32S3_EFUSE_DATE_WR_MASK     0x0fffffff
