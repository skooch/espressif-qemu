/*
 * T-Deck Pro SX1262 LoRa SPI slave model
 *
 * Emulates SX1262 SPI command protocol for QEMU simulation.
 * Supports init sequence, TX/RX with loopback, IRQ via DIO1/BUSY GPIOs.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 * License: GPLv2+
 */

#pragma once

#include "hw/qdev-core.h"
#include "qemu/timer.h"

#define TYPE_TDECK_LORA_SX1262 "tdeck-lora-sx1262"
#define TDECK_LORA_SX1262(obj) \
    OBJECT_CHECK(TdeckLoraSx1262State, (obj), TYPE_TDECK_LORA_SX1262)

/* SX1262 chip modes */
#define SX1262_MODE_SLEEP     0
#define SX1262_MODE_STDBY_RC  2
#define SX1262_MODE_STDBY_XOSC 3
#define SX1262_MODE_FS        4
#define SX1262_MODE_RX        5
#define SX1262_MODE_TX        6

/* SPI protocol states */
typedef enum {
    SX1262_SPI_IDLE,
    SX1262_SPI_OPCODE,
    SX1262_SPI_PARAMS,
    SX1262_SPI_DATA,
    SX1262_SPI_RESPONSE,
} Sx1262SpiState;

typedef struct TdeckLoraSx1262State {
    DeviceState parent_obj;

    /* GPIO outputs to guest */
    qemu_irq dio1;      /* DIO1 interrupt (active HIGH) */
    qemu_irq busy;      /* BUSY indicator (active HIGH) */

    /* SX1262 chip state */
    uint8_t chip_mode;
    uint8_t cmd_status;     /* 2=DataAvailable, 6=CmdTxDone */
    uint8_t packet_type;    /* 0=GFSK, 1=LoRa */
    uint8_t regulator_mode; /* 0=LDO, 1=DCDC */

    /* Radio config (stored, not RF-simulated) */
    uint32_t frequency_hz;
    int8_t tx_power;
    uint8_t sf, bw, cr;
    uint16_t sync_word;
    uint16_t irq_mask;
    uint16_t dio1_mask;
    bool dio2_rf_switch;
    bool dio3_tcxo;
    bool boosted_rx;

    /* 256-byte buffer (same as real chip) */
    uint8_t buffer[256];
    uint8_t tx_base;
    uint8_t rx_base;

    /* Last TX for loopback */
    uint8_t last_tx_data[256];
    uint8_t last_tx_len;

    /* Packet params */
    uint8_t payload_len;
    uint8_t preamble_len_hi;
    uint8_t preamble_len_lo;

    /* SPI transaction state machine */
    Sx1262SpiState spi_state;
    uint8_t opcode;
    uint8_t param_buf[16];
    uint8_t param_idx;
    uint8_t param_expected;
    uint8_t resp_buf[16];
    uint8_t resp_idx;
    uint8_t resp_len;
    uint8_t data_offset;
    uint16_t data_count;
    bool in_write_buffer;
    bool in_read_buffer;

    /* IRQ state */
    uint16_t irq_status;

    /* Timers */
    QEMUTimer *tx_done_timer;
    QEMUTimer *rx_loopback_timer;
    QEMUTimer *busy_timer;

    /* Loopback mode */
    bool loopback_enabled;
} TdeckLoraSx1262State;

/* Process one SPI byte exchange. Returns MISO byte. */
uint8_t tdeck_lora_spi_transfer(TdeckLoraSx1262State *s, uint8_t mosi);

/* Reset handler (called when RST GPIO goes LOW). */
void tdeck_lora_reset(TdeckLoraSx1262State *s);
