/*
 * ESP32-C3/S3 USB Serial JTAG emulation
 *
 * Implements the TX path used by esp-println / defmt:
 *   - Write bytes to EP1_REG (0x00) one at a time
 *   - Read EP1_CONF_REG (0x04) bit 1 (SERIAL_IN_EP_DATA_FREE) to check FIFO space
 *   - Write EP1_CONF_REG bit 0 (WR_DONE) to flush the buffer
 *
 * The buffer is flushed to a QEMU CharBackend (typically connected to stdio
 * via -nographic or to a TCP socket via -serial).
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties-system.h"
#include "hw/misc/esp32c3_jtag.h"

static void esp32c3_jtag_flush(ESP32C3UsbJtagState *s);

static void esp32c3_jtag_update_irq(ESP32C3UsbJtagState *s)
{
    qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
}

static int esp32c3_jtag_can_receive(void *opaque)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(opaque);

    return fifo8_num_free(&s->rx_fifo);
}

static void esp32c3_jtag_receive(void *opaque, const uint8_t *buf, int size)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(opaque);
    bool was_empty = fifo8_is_empty(&s->rx_fifo);

    for (int i = 0; i < size && fifo8_num_free(&s->rx_fifo) > 0; i++) {
        fifo8_push(&s->rx_fifo, buf[i]);
    }

    if (was_empty && fifo8_num_used(&s->rx_fifo) > 0) {
        s->int_raw |= USB_SERIAL_JTAG_INT_RX_AVAIL;
        esp32c3_jtag_update_irq(s);
    }
}

static void esp32c3_jtag_flush(ESP32C3UsbJtagState *s)
{
    if (s->tx_buf_pos == 0) {
        return;
    }

    if (qemu_chr_fe_backend_connected(&s->chr)) {
        /* Write all buffered bytes to the chardev backend */
        qemu_chr_fe_write_all(&s->chr, s->tx_buf, s->tx_buf_pos);
    }

    s->tx_buf_pos = 0;
    s->int_raw |= USB_SERIAL_JTAG_INT_TX_DONE;
    esp32c3_jtag_update_irq(s);
}

static uint64_t esp32c3_jtag_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(opaque);

    switch (addr) {
    case USB_SERIAL_JTAG_EP1_REG:
        if (fifo8_num_used(&s->rx_fifo) == 0) {
            return 0;
        }
        {
            uint8_t byte = fifo8_pop(&s->rx_fifo);
            if (fifo8_is_empty(&s->rx_fifo)) {
                s->int_raw &= ~USB_SERIAL_JTAG_INT_RX_AVAIL;
                esp32c3_jtag_update_irq(s);
            }
            qemu_chr_fe_accept_input(&s->chr);
            return byte;
        }

    case USB_SERIAL_JTAG_EP1_CONF_REG:
        return (s->tx_buf_pos < USB_SERIAL_JTAG_TX_BUF_SIZE ? 0x02 : 0) |
               (fifo8_num_used(&s->rx_fifo) > 0 ? 0x04 : 0);

    case USB_SERIAL_JTAG_INT_RAW_REG:
        return s->int_raw;

    case USB_SERIAL_JTAG_INT_ST_REG:
        return s->int_raw & s->int_ena;

    case USB_SERIAL_JTAG_INT_ENA_REG:
        return s->int_ena;

    default:
        return 0;
    }
}

static void esp32c3_jtag_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(opaque);

    switch (addr) {
    case USB_SERIAL_JTAG_EP1_REG:
        if (s->tx_buf_pos >= USB_SERIAL_JTAG_TX_BUF_SIZE) {
            esp32c3_jtag_flush(s);
        }

        /* Append byte to TX buffer */
        if (s->tx_buf_pos < USB_SERIAL_JTAG_TX_BUF_SIZE) {
            s->tx_buf[s->tx_buf_pos++] = (uint8_t)(value & 0xFF);
        }
        break;

    case USB_SERIAL_JTAG_EP1_CONF_REG:
        /* Bit 0 (WR_DONE): flush the TX buffer */
        if (value & 0x01) {
            esp32c3_jtag_flush(s);
        }
        break;

    case USB_SERIAL_JTAG_INT_ENA_REG:
        s->int_ena = (uint32_t)value;
        esp32c3_jtag_update_irq(s);
        break;

    case USB_SERIAL_JTAG_INT_CLR_REG:
        /* Write-1-to-clear */
        s->int_raw &= ~(uint32_t)value;
        esp32c3_jtag_update_irq(s);
        break;

    default:
        break;
    }
}

static const MemoryRegionOps esp32c3_jtag_ops = {
    .read =  esp32c3_jtag_read,
    .write = esp32c3_jtag_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32c3_jtag_reset_hold(Object *obj, ResetType type)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(obj);
    (void) type;

    s->tx_buf_pos = 0;
    s->int_raw = 0;
    s->int_ena = 0;
    fifo8_reset(&s->rx_fifo);
    qemu_irq_lower(s->irq);
}

static void esp32c3_jtag_realize(DeviceState *dev, Error **errp)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(dev);

    qemu_chr_fe_set_handlers(&s->chr, esp32c3_jtag_can_receive,
                             esp32c3_jtag_receive, NULL, NULL,
                             s, NULL, true);
    (void) errp;
}

static void esp32c3_jtag_init(Object *obj)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_jtag_ops, s,
                          TYPE_ESP32C3_JTAG, ESP32C3_JTAG_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    fifo8_create(&s->rx_fifo, USB_SERIAL_JTAG_RX_BUF_SIZE);
}

static Property esp32c3_jtag_properties[] = {
    DEFINE_PROP_CHR("chardev", ESP32C3UsbJtagState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32c3_jtag_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32c3_jtag_reset_hold;
    dc->realize = esp32c3_jtag_realize;
    device_class_set_props(dc, esp32c3_jtag_properties);
}

static const TypeInfo esp32c3_jtag_info = {
    .name = TYPE_ESP32C3_JTAG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3UsbJtagState),
    .instance_init = esp32c3_jtag_init,
    .class_init = esp32c3_jtag_class_init
};

static void esp32c3_jtag_types(void)
{
    type_register_static(&esp32c3_jtag_info);
}

type_init(esp32c3_jtag_types)
