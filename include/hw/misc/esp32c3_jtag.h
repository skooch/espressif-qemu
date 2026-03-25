/*
 * ESP32-C3/S3 USB Serial JTAG emulation
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "chardev/char-fe.h"

#define TYPE_ESP32C3_JTAG "misc.esp32c3.usb_serial_jtag"
#define ESP32C3_JTAG(obj) OBJECT_CHECK(ESP32C3UsbJtagState, (obj), TYPE_ESP32C3_JTAG)

#define ESP32C3_JTAG_REGS_SIZE (0x84)

/* Register offsets */
#define USB_SERIAL_JTAG_EP1_REG         0x00  /* TX FIFO write */
#define USB_SERIAL_JTAG_EP1_CONF_REG    0x04  /* Bit 0: WR_DONE, Bit 1: DATA_FREE */
#define USB_SERIAL_JTAG_INT_RAW_REG     0x08
#define USB_SERIAL_JTAG_INT_ST_REG      0x0C
#define USB_SERIAL_JTAG_INT_ENA_REG     0x10
#define USB_SERIAL_JTAG_INT_CLR_REG     0x14

#define USB_SERIAL_JTAG_TX_BUF_SIZE     64

typedef struct ESP32C3UsbJtagState {
    SysBusDevice parent_object;
    MemoryRegion iomem;
    CharBackend chr;

    /* TX FIFO buffer */
    uint8_t tx_buf[USB_SERIAL_JTAG_TX_BUF_SIZE];
    uint32_t tx_buf_pos;

    /* Interrupt registers */
    uint32_t int_raw;
    uint32_t int_ena;

} ESP32C3UsbJtagState;
