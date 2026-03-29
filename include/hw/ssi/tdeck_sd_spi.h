/*
 * T-Deck Pro SD Card SPI slave model
 *
 * Translates SPI byte stream to/from QEMU SDState commands.
 * Wraps the built-in SD card model for block storage.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 * License: GPLv2+
 */

#pragma once

#include "hw/qdev-core.h"
#include "hw/sd/sd.h"

#define TYPE_TDECK_SD_SPI "tdeck-sd-spi"
#define TDECK_SD_SPI(obj) OBJECT_CHECK(TdeckSdSpiState, (obj), TYPE_TDECK_SD_SPI)

typedef enum {
    SD_SPI_IDLE,
    SD_SPI_RECEIVING_CMD,
    SD_SPI_RESPONDING,
    SD_SPI_READING_DATA,
    SD_SPI_WAIT_WRITE_TOKEN,
    SD_SPI_RECEIVING_WRITE,
    SD_SPI_BUSY,
} SdSpiProtoState;

typedef struct TdeckSdSpiState {
    DeviceState parent_obj;

    SDState *sd;                /* QEMU built-in SD card model (NULL if no drive) */
    bool inserted;              /* Card present (toggled by panel) */

    /* SPI protocol state machine */
    SdSpiProtoState state;

    uint8_t cmd_buf[6];        /* Command frame accumulator */
    uint8_t cmd_idx;

    uint8_t resp_buf[17];      /* Response buffer (R1 + up to 16 trailing bytes) */
    uint8_t resp_len;
    uint8_t resp_idx;

    uint8_t data_buf[515];     /* token + 512 data + 2 CRC */
    uint16_t data_len;
    uint16_t data_idx;

    bool app_cmd;              /* CMD55 prefix active */
    uint8_t busy_cycles;       /* Remaining busy (0x00) bytes after write */
} TdeckSdSpiState;

/* Process one SPI byte exchange. Returns MISO byte. */
uint8_t tdeck_sd_spi_transfer(TdeckSdSpiState *s, uint8_t mosi);

/* Set card insertion state. Resets protocol state on eject. */
void tdeck_sd_spi_set_inserted(TdeckSdSpiState *s, bool inserted);
