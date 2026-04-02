#pragma once

#include "hw/sysbus.h"

#define TYPE_ESP32S3_IOMUX "esp32s3.iomux"
#define ESP32S3_IOMUX(obj) \
    OBJECT_CHECK(ESP32S3IOMuxState, (obj), TYPE_ESP32S3_IOMUX)

#define ESP32S3_IOMUX_IO_SIZE 0x2000
#define ESP32S3_IOMUX_REG_COUNT (ESP32S3_IOMUX_IO_SIZE / sizeof(uint32_t))
#define ESP32S3_IOMUX_GPIO_REG(pin) ((pin) * sizeof(uint32_t))

#define ESP32S3_IOMUX_MCU_SEL_SHIFT 12
#define ESP32S3_IOMUX_MCU_SEL_MASK (0x7u << ESP32S3_IOMUX_MCU_SEL_SHIFT)

typedef struct ESP32S3GPIOState ESP32S3GPIOState;

typedef struct ESP32S3IOMuxState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[ESP32S3_IOMUX_REG_COUNT];

    ESP32S3GPIOState *gpio;
} ESP32S3IOMuxState;
