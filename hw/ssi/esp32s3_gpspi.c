/*
 * ESP32-S3 General Purpose SPI (SPI2/SPI3) controller emulation
 *
 * Implements the minimum register interface needed for esp-hal SPI master:
 *   - CMD register: USR bit (start transfer, clears after completion delay)
 *                   UPDATE bit (register sync, auto-clears)
 *   - DMA_INT registers: TRANS_DONE status/enable/clear for completion signaling
 *   - All other registers: store and reflect writes
 *
 * Transfers complete without bus timing emulation. The USR bit
 * stays asserted while a transfer window is active, then TRANS_DONE is set in
 * DMA_INT_RAW when done.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/ssi/esp32s3_gpspi.h"
#include "hw/dma/esp_gdma.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/display/tdeck_uc8253.h"
#include "hw/ssi/tdeck_sd_spi.h"
#include "hw/ssi/tdeck_lora_sx1262.h"

#define ESP32S3_GPSPI(obj) OBJECT_CHECK(Esp32s3GpSpiState, (obj), TYPE_ESP32S3_GPSPI)

/* Register offsets */
#define SPI_CMD_REG         0x00
#define SPI_CTRL_REG        0x08
#define SPI_CLOCK_REG       0x0C
#define SPI_USER_REG        0x10
#define SPI_USER1_REG       0x14
#define SPI_USER2_REG       0x18
#define SPI_MS_DLEN_REG     0x1C
#define SPI_MISC_REG        0x20
#define SPI_DMA_CONF_REG    0x30
#define SPI_DMA_INT_ENA_REG 0x34
#define SPI_DMA_INT_CLR_REG 0x38
#define SPI_DMA_INT_RAW_REG 0x3C
#define SPI_DMA_INT_ST_REG  0x40
#define SPI_DMA_INT_SET_REG 0x44
#define SPI_W0_REG          0x98
#define SPI_W15_REG         0xD4
#define SPI_SLAVE_REG       0xE0
#define SPI_CLK_GATE_REG    0xE8
#define SPI_DATE_REG        0xF0

/* CMD register bits */
#define SPI_CMD_UPDATE_BIT  (1 << 23)
#define SPI_CMD_USR_BIT     (1 << 24)

/* USER register bits */
#define SPI_USR_MOSI_BIT    (1 << 27)
#define SPI_USR_MISO_BIT    (1 << 28)

/* DMA_INT bit positions */
#define SPI_INT_TRANS_DONE  (1 << 12)

/* Region covers all registers up to 0xF4 */
#define ESP32S3_GPSPI_IO_SIZE 0x100

/* Generic register storage (0x100 / 4 = 64 words) */
#define ESP32S3_GPSPI_REG_COUNT (ESP32S3_GPSPI_IO_SIZE / 4)
#define ESP32S3_GPSPI_TRANSFER_DELAY_NS 1200
#define ESP32S3_GPSPI_EXECUTE_DELAY_NS 800

static void esp32s3_gpspi_execute_transfer(Esp32s3GpSpiState *s);

static void esp32s3_gpspi_update_irq(Esp32s3GpSpiState *s)
{
    uint32_t raw = s->regs[SPI_DMA_INT_RAW_REG / 4];
    uint32_t ena = s->regs[SPI_DMA_INT_ENA_REG / 4];
    uint32_t st = raw & ena;
    s->regs[SPI_DMA_INT_ST_REG / 4] = st;
    qemu_set_irq(s->irq, st != 0);
}

static bool esp32s3_gpspi_signal_asserted(Esp32s3GpSpiState *s, uint32_t signal)
{
    bool level = true;

    if (!s->gpio || !esp32s3_gpio_get_routed_signal_level(s->gpio, signal, &level)) {
        return false;
    }

    return !level;
}

static void esp32s3_gpspi_finish_transfer(void *opaque)
{
    Esp32s3GpSpiState *s = ESP32S3_GPSPI(opaque);

    if (!s->transfer_data_executed) {
        s->transfer_data_executed = true;
        esp32s3_gpspi_execute_transfer(s);
        timer_mod_ns(&s->completion_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     ESP32S3_GPSPI_TRANSFER_DELAY_NS);
        return;
    }

    s->transfer_in_progress = false;
    s->transfer_data_executed = false;
    s->regs[SPI_CMD_REG / 4] &= ~SPI_CMD_USR_BIT;
    s->regs[SPI_DMA_INT_RAW_REG / 4] |= SPI_INT_TRANS_DONE;
    esp32s3_gpspi_update_irq(s);
}

static void esp32s3_gpspi_execute_transfer(Esp32s3GpSpiState *s)
{
    /* Pull DMA TX data and route to EPD slave if CS is asserted */
    if (s->gdma && s->gpio && s->epd) {
        bool cs_low = esp32s3_gpspi_signal_asserted(s, ESP32S3_GPIO_SIG_EPD_CS);
        if (cs_low) {
            uint32_t ms_dlen = s->regs[SPI_MS_DLEN_REG / 4];
            uint32_t byte_count = (ms_dlen + 1) / 8;
            if (byte_count > 0 && byte_count <= 16384) {
                uint32_t chan;
                if (esp_gdma_get_channel_periph(s->gdma,
                        (GdmaPeripheral)s->gdma_periph_id,
                        ESP_GDMA_OUT_IDX, &chan)) {
                    bool dc = false;
                    esp32s3_gpio_get_routed_signal_level(s->gpio,
                                                         ESP32S3_GPIO_SIG_EPD_DC,
                                                         &dc);
                    uint8_t *buf = g_malloc(byte_count);
                    if (esp_gdma_read_channel_data(s->gdma, chan,
                                                   buf, byte_count)) {
                        tdeck_uc8253_spi_receive(s->epd, buf,
                                                 byte_count, dc);
                    }
                    g_free(buf);
                }
            }
        }
    }

    if (s->gdma && s->gpio && s->sd_spi) {
        bool sd_cs = esp32s3_gpspi_signal_asserted(s, ESP32S3_GPIO_SIG_SD_CS);
        if (sd_cs) {
            uint32_t ms_dlen = s->regs[SPI_MS_DLEN_REG / 4];
            uint32_t byte_count = (ms_dlen + 1) / 8;
            if (byte_count > 0 && byte_count <= 16384) {
                uint32_t tx_chan, rx_chan;
                bool has_tx = esp_gdma_get_channel_periph(s->gdma,
                    (GdmaPeripheral)s->gdma_periph_id,
                    ESP_GDMA_OUT_IDX, &tx_chan);
                bool has_rx = esp_gdma_get_channel_periph(s->gdma,
                    (GdmaPeripheral)s->gdma_periph_id,
                    ESP_GDMA_IN_IDX, &rx_chan);

                uint8_t *tx_buf = g_malloc(byte_count);
                uint8_t *rx_buf = g_malloc0(byte_count);
                uint32_t user_reg = s->regs[SPI_USER_REG / 4];
                bool mosi_active = user_reg & SPI_USR_MOSI_BIT;
                bool miso_active = user_reg & SPI_USR_MISO_BIT;

                if (has_tx && mosi_active) {
                    if (!esp_gdma_read_channel_data(s->gdma, tx_chan,
                                                    tx_buf, byte_count)) {
                        memset(tx_buf, 0xFF, byte_count);
                    }
                } else {
                    memset(tx_buf, 0xFF, byte_count);
                }

                for (uint32_t i = 0; i < byte_count; i++) {
                    rx_buf[i] = tdeck_sd_spi_transfer(s->sd_spi, tx_buf[i]);
                }

                if (has_rx && miso_active) {
                    esp_gdma_write_channel(s->gdma, rx_chan, rx_buf, byte_count);
                }

                g_free(tx_buf);
                g_free(rx_buf);
            }
        }
    }

    if (s->gdma && s->gpio && s->lora) {
        bool lora_cs = esp32s3_gpspi_signal_asserted(s, ESP32S3_GPIO_SIG_LORA_CS);
        if (lora_cs) {
            uint32_t ms_dlen = s->regs[SPI_MS_DLEN_REG / 4];
            uint32_t byte_count = (ms_dlen + 1) / 8;
            if (byte_count > 0 && byte_count <= 16384) {
                uint32_t tx_chan, rx_chan;
                bool has_tx = esp_gdma_get_channel_periph(s->gdma,
                    (GdmaPeripheral)s->gdma_periph_id,
                    ESP_GDMA_OUT_IDX, &tx_chan);
                bool has_rx = esp_gdma_get_channel_periph(s->gdma,
                    (GdmaPeripheral)s->gdma_periph_id,
                    ESP_GDMA_IN_IDX, &rx_chan);

                uint8_t *tx_buf = g_malloc(byte_count);
                uint8_t *rx_buf = g_malloc0(byte_count);
                uint32_t user_reg = s->regs[SPI_USER_REG / 4];
                bool mosi_active = user_reg & SPI_USR_MOSI_BIT;
                bool miso_active = user_reg & SPI_USR_MISO_BIT;

                if (has_tx && mosi_active) {
                    if (!esp_gdma_read_channel_data(s->gdma, tx_chan,
                                                    tx_buf, byte_count)) {
                        memset(tx_buf, 0x00, byte_count);
                    }
                } else {
                    memset(tx_buf, 0x00, byte_count);
                }

                for (uint32_t i = 0; i < byte_count; i++) {
                    rx_buf[i] = tdeck_lora_spi_transfer(s->lora, tx_buf[i]);
                }

                if (has_rx && miso_active) {
                    esp_gdma_write_channel(s->gdma, rx_chan, rx_buf, byte_count);
                }

                g_free(tx_buf);
                g_free(rx_buf);
            }
        }
    }
}

static uint64_t esp32s3_gpspi_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3GpSpiState *s = ESP32S3_GPSPI(opaque);

    if (addr / 4 < ESP32S3_GPSPI_REG_COUNT) {
        return s->regs[addr / 4];
    }
    return 0;
}

static void esp32s3_gpspi_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned int size)
{
    Esp32s3GpSpiState *s = ESP32S3_GPSPI(opaque);

    switch (addr) {
    case SPI_CMD_REG:
        /*
         * USR (bit 24): start a transfer. On real hardware, the SPI controller
         * executes the configured transfer and clears USR when done. USR remains
         * asserted until the completion window ends.
         *
         * UPDATE (bit 23): register sync. Auto-clears when sync is done. We
         * clear it instantly.
         */
        if ((value & SPI_CMD_USR_BIT) && !s->transfer_in_progress) {
            s->transfer_in_progress = true;
            s->regs[SPI_DMA_INT_RAW_REG / 4] &= ~SPI_INT_TRANS_DONE;
            s->transfer_data_executed = false;
            s->regs[SPI_CMD_REG / 4] = (uint32_t)value;
            timer_mod_ns(&s->completion_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         ESP32S3_GPSPI_EXECUTE_DELAY_NS);
        }
        if (value & SPI_CMD_UPDATE_BIT) {
            value &= ~SPI_CMD_UPDATE_BIT;
        }
        if (!s->transfer_in_progress) {
            s->regs[SPI_CMD_REG / 4] = (uint32_t)value;
        } else {
            s->regs[SPI_CMD_REG / 4] &= ~SPI_CMD_UPDATE_BIT;
        }
        break;

    case SPI_DMA_INT_ENA_REG:
        s->regs[SPI_DMA_INT_ENA_REG / 4] = (uint32_t)value;
        esp32s3_gpspi_update_irq(s);
        break;

    case SPI_DMA_INT_CLR_REG:
        /* Write-1-to-clear */
        s->regs[SPI_DMA_INT_RAW_REG / 4] &= ~(uint32_t)value;
        esp32s3_gpspi_update_irq(s);
        break;

    case SPI_DMA_INT_SET_REG:
        /* Write-1-to-set (software trigger) */
        s->regs[SPI_DMA_INT_RAW_REG / 4] |= (uint32_t)value;
        esp32s3_gpspi_update_irq(s);
        break;

    default:
        /* Store all other register writes for read-back */
        if (addr / 4 < ESP32S3_GPSPI_REG_COUNT) {
            s->regs[addr / 4] = (uint32_t)value;
        }
        break;
    }
}

static const MemoryRegionOps esp32s3_gpspi_ops = {
    .read  = esp32s3_gpspi_read,
    .write = esp32s3_gpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_gpspi_reset_hold(Object *obj, ResetType type)
{
    Esp32s3GpSpiState *s = ESP32S3_GPSPI(obj);
    memset(s->regs, 0, sizeof(s->regs));
    s->transfer_in_progress = false;
    s->transfer_data_executed = false;
    timer_del(&s->completion_timer);
}

static void esp32s3_gpspi_realize(DeviceState *dev, Error **errp)
{
}

static void esp32s3_gpspi_init(Object *obj)
{
    Esp32s3GpSpiState *s = ESP32S3_GPSPI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_gpspi_ops, s,
                          TYPE_ESP32S3_GPSPI, ESP32S3_GPSPI_IO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    timer_init_ns(&s->completion_timer, QEMU_CLOCK_VIRTUAL,
                  esp32s3_gpspi_finish_transfer, s);
}

static void esp32s3_gpspi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_gpspi_reset_hold;
    dc->realize = esp32s3_gpspi_realize;
}

static const TypeInfo esp32s3_gpspi_info = {
    .name = TYPE_ESP32S3_GPSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3GpSpiState),
    .instance_init = esp32s3_gpspi_init,
    .class_init = esp32s3_gpspi_class_init,
};

static void esp32s3_gpspi_register_types(void)
{
    type_register_static(&esp32s3_gpspi_info);
}

type_init(esp32s3_gpspi_register_types)
