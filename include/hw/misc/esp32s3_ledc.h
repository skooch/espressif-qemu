#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_ESP32S3_LEDC "misc.esp32s3.ledc"
#define ESP32S3_LEDC(obj) OBJECT_CHECK(ESP32S3LEDCState, (obj), TYPE_ESP32S3_LEDC)

typedef struct ESP32S3LEDCState {
    SysBusDevice parent_object;
    MemoryRegion iomem;
    uint32_t regs[0x100 / sizeof(uint32_t)];
} ESP32S3LEDCState;
