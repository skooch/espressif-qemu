#pragma once

#include "hw/sysbus.h"

#define TYPE_ESP32S3_GPSPI "esp32s3.gpspi"

typedef struct Esp32s3GpSpiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x100 / 4];
} Esp32s3GpSpiState;
