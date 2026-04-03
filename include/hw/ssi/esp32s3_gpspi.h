#pragma once

#include "hw/sysbus.h"
#include "qemu/timer.h"

#define TYPE_ESP32S3_GPSPI "esp32s3.gpspi"

/* Forward declarations */
typedef struct ESPGdmaState ESPGdmaState;
typedef struct ESP32S3GPIOState ESP32S3GPIOState;
typedef struct TdeckUc8253State TdeckUc8253State;
typedef struct TdeckSdSpiState TdeckSdSpiState;
typedef struct TdeckLoraSx1262State TdeckLoraSx1262State;

typedef struct Esp32s3GpSpiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer completion_timer;
    uint32_t regs[0x100 / 4];
    bool transfer_in_progress;
    bool transfer_data_executed;
    uint8_t transfer_retry_count;
    bool tx_fifo_dirty;
    bool transfer_prefers_fifo;

    /* References for SPI slave data routing */
    ESPGdmaState *gdma;
    ESP32S3GPIOState *gpio;
    TdeckUc8253State *epd;       /* UC8253 EPD slave (optional) */
    TdeckSdSpiState *sd_spi;     /* SD card SPI slave (optional) */
    TdeckLoraSx1262State *lora;  /* SX1262 LoRa SPI slave (optional) */
    int gdma_periph_id;          /* GDMA peripheral ID for this SPI (0=SPI2, 1=SPI3) */
} Esp32s3GpSpiState;
