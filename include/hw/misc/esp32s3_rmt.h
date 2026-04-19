#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_ESP32S3_RMT "misc.esp32s3.rmt"
#define ESP32S3_RMT(obj) OBJECT_CHECK(ESP32S3RMTState, (obj), TYPE_ESP32S3_RMT)

typedef struct ESP32S3RMTState {
    SysBusDevice parent_object;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0xd0 / sizeof(uint32_t)];
} ESP32S3RMTState;
