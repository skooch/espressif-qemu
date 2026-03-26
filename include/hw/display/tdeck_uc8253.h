/*
 * T-Deck Pro UC8253 EPD display model
 *
 * Copyright (c) 2026 T-Deck Pro Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/qdev-core.h"
#include "ui/console.h"
#include "qemu/timer.h"

#define TYPE_TDECK_UC8253 "tdeck-uc8253"
#define TDECK_UC8253(obj) OBJECT_CHECK(TdeckUc8253State, (obj), TYPE_TDECK_UC8253)

#define UC8253_WIDTH  240
#define UC8253_HEIGHT 320
#define UC8253_BUF_SIZE (UC8253_WIDTH * UC8253_HEIGHT / 8)  /* 9600 bytes */

/* UC8253 commands */
#define UC8253_CMD_PSR          0x00
#define UC8253_CMD_POWER_OFF    0x02
#define UC8253_CMD_POWER_ON     0x04
#define UC8253_CMD_DTM1         0x10  /* Old data */
#define UC8253_CMD_REFRESH      0x12  /* Display Refresh */
#define UC8253_CMD_DTM2         0x13  /* New data */
#define UC8253_CMD_PARTIAL_WIN  0x90  /* Partial Window (7 bytes data) */
#define UC8253_CMD_PARTIAL_IN   0x91
#define UC8253_CMD_PARTIAL_OUT  0x92

typedef struct TdeckUc8253State {
    DeviceState parent_obj;

    QemuConsole *con;

    /* Command state machine */
    uint8_t current_cmd;
    uint32_t data_idx;         /* Byte index within current command data */
    bool power_on;
    bool partial_mode;

    /* Partial window */
    uint16_t partial_x_start;
    uint16_t partial_x_end;
    uint16_t partial_y_start;
    uint16_t partial_y_end;

    /* Pixel buffers */
    uint8_t previous[UC8253_BUF_SIZE];
    uint8_t current[UC8253_BUF_SIZE];

    /* Partial window data accumulator */
    uint8_t pw_buf[7];

    /* BUSY pin callback */
    qemu_irq busy_pin;
    QEMUTimer *busy_timer;
} TdeckUc8253State;

/* Called by GP-SPI model to deliver SPI bytes */
void tdeck_uc8253_spi_receive(TdeckUc8253State *s, const uint8_t *data,
                               uint32_t len, bool dc_level);
