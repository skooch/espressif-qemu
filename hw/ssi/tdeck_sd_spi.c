/*
 * T-Deck Pro SD Card SPI slave model
 *
 * Translates SPI byte stream to/from QEMU SDState commands.
 * Wraps the built-in SD card model for block storage.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 * License: GPLv2+
 */

#include "qemu/osdep.h"
#include "hw/ssi/tdeck_sd_spi.h"
#include "hw/sd/sd.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "qapi/error.h"

/* #define DEBUG_TDECK_SD 1 */

#ifdef DEBUG_TDECK_SD
#define DPRINTF(fmt, ...) \
    qemu_log("tdeck-sd-spi: " fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define SD_SPI_IDLE_BYTE    0xFF
#define SD_SPI_DATA_TOKEN   0xFE
#define SD_SPI_DATA_ACCEPTED 0x05
#define SD_SPI_BUSY_CYCLES  4

static void reset_state(TdeckSdSpiState *s)
{
    s->state = SD_SPI_IDLE;
    s->cmd_idx = 0;
    s->resp_len = 0;
    s->resp_idx = 0;
    s->data_len = 0;
    s->data_idx = 0;
    s->app_cmd = false;
    s->busy_cycles = 0;
}

static int sd_card_do_command(SDState *sd, SDRequest *req, uint8_t *response)
{
    SDCardClass *sc = SD_CARD_GET_CLASS(sd);
    return sc->do_command(sd, req, response);
}

static uint8_t sd_card_read_byte(SDState *sd)
{
    SDCardClass *sc = SD_CARD_GET_CLASS(sd);
    return sc->read_byte(sd);
}

static void sd_card_write_byte(SDState *sd, uint8_t value)
{
    SDCardClass *sc = SD_CARD_GET_CLASS(sd);
    sc->write_byte(sd, value);
}

static bool sd_card_data_ready(SDState *sd)
{
    SDCardClass *sc = SD_CARD_GET_CLASS(sd);
    return sc->data_ready(sd);
}

/*
 * Build the SPI R1 byte from the card status returned by sd_card_do_command.
 * In idle state (before ACMD41 completes), bit 0 is set.
 */
static uint8_t make_r1(uint8_t *response, int rsplen, bool in_idle)
{
    uint8_t r1 = 0;

    if (in_idle) {
        r1 |= 0x01;  /* idle state */
    }

    if (rsplen >= 4) {
        uint32_t cs = ((uint32_t)response[0] << 24) |
                      ((uint32_t)response[1] << 16) |
                      ((uint32_t)response[2] << 8)  |
                      response[3];
        if (cs & ILLEGAL_COMMAND) {
            r1 |= 0x04;
        }
        if (cs & COM_CRC_ERROR) {
            r1 |= 0x08;
        }
        if (cs & ERASE_SEQ_ERROR) {
            r1 |= 0x10;
        }
        if (cs & ADDRESS_ERROR) {
            r1 |= 0x20;
        }
        if (cs & (OUT_OF_RANGE | CID_CSD_OVERWRITE)) {
            r1 |= 0x40;
        }
    }

    return r1;
}

/*
 * Process a complete 6-byte command frame.
 * Populates resp_buf/resp_len and decides the next state.
 */
static void process_command(TdeckSdSpiState *s)
{
    uint8_t cmd = s->cmd_buf[0] & 0x3F;
    uint32_t arg = ((uint32_t)s->cmd_buf[1] << 24) |
                   ((uint32_t)s->cmd_buf[2] << 16) |
                   ((uint32_t)s->cmd_buf[3] << 8)  |
                   s->cmd_buf[4];
    SDRequest req;
    uint8_t response[16];
    int rsplen;
    bool was_app_cmd = s->app_cmd;
    bool card_ready;

    s->app_cmd = false;

    if (!s->sd) {
        /* No card backend, return illegal command */
        s->resp_buf[0] = 0x04;
        s->resp_len = 1;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        return;
    }

    req.cmd = cmd;
    req.arg = arg;
    req.crc = s->cmd_buf[5];

    DPRINTF("%sCMD%d arg=0x%08x\n", was_app_cmd ? "A" : "", cmd, arg);

    if (cmd == 55) {
        /* CMD55: next command is app command */
        rsplen = sd_card_do_command(s->sd, &req, response);
        (void)rsplen;
        s->app_cmd = true;
        /* R1 response */
        s->resp_buf[0] = 0x01; /* idle */
        s->resp_len = 1;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        return;
    }

    /* Forward command to SDState */
    rsplen = sd_card_do_command(s->sd, &req, response);

    if (rsplen <= 0) {
        /* Command failed */
        s->resp_buf[0] = 0x04; /* illegal command */
        s->resp_len = 1;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        DPRINTF("CMD%d failed (rsplen=%d)\n", cmd, rsplen);
        return;
    }

    /*
     * Determine if card is in idle state.
     * After successful ACMD41, card transitions out of idle.
     */
    if (was_app_cmd && cmd == 41) {
        /* ACMD41: check if card is ready from the OCR */
        card_ready = (rsplen >= 4) &&
                     (response[0] & 0x80); /* busy bit set = ready */
        s->resp_buf[0] = card_ready ? 0x00 : 0x01;
        s->resp_len = 1;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        DPRINTF("ACMD41: card_ready=%d\n", card_ready);
        return;
    }

    /* Check idle state from card status */
    bool in_idle = (rsplen >= 4) &&
                   (((((uint32_t)response[0] << 24) |
                      ((uint32_t)response[1] << 16) |
                      ((uint32_t)response[2] << 8) |
                      response[3]) >> 9) & 0xF) < 4;

    switch (cmd) {
    case 8:
        /* CMD8 (SEND_IF_COND): R7 = R1 + 4 bytes */
        s->resp_buf[0] = make_r1(response, rsplen, in_idle);
        if (rsplen >= 4) {
            memcpy(&s->resp_buf[1], response, 4);
        } else {
            memset(&s->resp_buf[1], 0, 4);
        }
        s->resp_len = 5;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        break;

    case 58:
        /* CMD58 (READ_OCR): R3 = R1 + 4 bytes OCR */
        s->resp_buf[0] = make_r1(response, rsplen, in_idle);
        if (rsplen >= 4) {
            memcpy(&s->resp_buf[1], response, 4);
        } else {
            memset(&s->resp_buf[1], 0, 4);
        }
        s->resp_len = 5;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        break;

    case 9:  /* CMD9 (SEND_CSD) */
    case 17: /* CMD17 (READ_SINGLE_BLOCK) */
        /* R1 response, then data phase */
        s->resp_buf[0] = make_r1(response, rsplen, in_idle);
        s->resp_len = 1;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        /* Data will be read in READING_DATA state */
        break;

    case 24: /* CMD24 (WRITE_BLOCK) */
        s->resp_buf[0] = make_r1(response, rsplen, in_idle);
        s->resp_len = 1;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        break;

    default:
        /* Generic R1 response */
        s->resp_buf[0] = make_r1(response, rsplen, in_idle);
        s->resp_len = 1;
        s->resp_idx = 0;
        s->state = SD_SPI_RESPONDING;
        break;
    }
}

/*
 * Determine next state after response is fully sent,
 * based on the command that was just processed.
 */
static SdSpiProtoState post_response_state(TdeckSdSpiState *s)
{
    uint8_t cmd = s->cmd_buf[0] & 0x3F;

    switch (cmd) {
    case 9:  /* SEND_CSD: 16 bytes of data */
    case 17: /* READ_SINGLE_BLOCK: 512 bytes */
        if (s->sd && sd_card_data_ready(s->sd)) {
            return SD_SPI_READING_DATA;
        }
        return SD_SPI_IDLE;

    case 24: /* WRITE_BLOCK */
        return SD_SPI_WAIT_WRITE_TOKEN;

    default:
        return SD_SPI_IDLE;
    }
}

uint8_t tdeck_sd_spi_transfer(TdeckSdSpiState *s, uint8_t mosi)
{
    if (!s->inserted || !s->sd) {
        return SD_SPI_IDLE_BYTE;
    }

    switch (s->state) {
    case SD_SPI_IDLE:
        if ((mosi & 0xC0) == 0x40) {
            /* Command start byte: 0b01xxxxxx */
            s->cmd_buf[0] = mosi;
            s->cmd_idx = 1;
            s->state = SD_SPI_RECEIVING_CMD;
        }
        return SD_SPI_IDLE_BYTE;

    case SD_SPI_RECEIVING_CMD:
        s->cmd_buf[s->cmd_idx++] = mosi;
        if (s->cmd_idx >= 6) {
            process_command(s);
        }
        return SD_SPI_IDLE_BYTE;

    case SD_SPI_RESPONDING:
        if (s->resp_idx < s->resp_len) {
            return s->resp_buf[s->resp_idx++];
        }
        /* Response fully sent, transition */
        s->state = post_response_state(s);
        if (s->state == SD_SPI_READING_DATA) {
            /* Prepare data buffer: token + data + CRC */
            uint8_t cmd = s->cmd_buf[0] & 0x3F;
            uint16_t data_bytes = (cmd == 9) ? 16 : 512;
            uint16_t i;

            s->data_buf[0] = SD_SPI_DATA_TOKEN;
            for (i = 0; i < data_bytes; i++) {
                s->data_buf[1 + i] = sd_card_read_byte(s->sd);
            }
            /* CRC16 placeholder (not checked by most firmware) */
            s->data_buf[1 + data_bytes] = 0x00;
            s->data_buf[2 + data_bytes] = 0x00;
            s->data_len = 1 + data_bytes + 2;
            s->data_idx = 0;
        }
        return SD_SPI_IDLE_BYTE;

    case SD_SPI_READING_DATA:
        if (s->data_idx < s->data_len) {
            return s->data_buf[s->data_idx++];
        }
        s->state = SD_SPI_IDLE;
        return SD_SPI_IDLE_BYTE;

    case SD_SPI_WAIT_WRITE_TOKEN:
        if (mosi == SD_SPI_DATA_TOKEN) {
            s->data_idx = 0;
            s->data_len = 514; /* 512 data + 2 CRC */
            s->state = SD_SPI_RECEIVING_WRITE;
        }
        return SD_SPI_IDLE_BYTE;

    case SD_SPI_RECEIVING_WRITE:
        if (s->data_idx < 512) {
            s->data_buf[s->data_idx] = mosi;
        }
        /* bytes 512-513 are CRC, ignored */
        s->data_idx++;
        if (s->data_idx >= s->data_len) {
            /* Write data to SD card */
            uint16_t i;
            for (i = 0; i < 512; i++) {
                sd_card_write_byte(s->sd, s->data_buf[i]);
            }
            s->busy_cycles = SD_SPI_BUSY_CYCLES;
            s->state = SD_SPI_BUSY;
            return SD_SPI_DATA_ACCEPTED;
        }
        return SD_SPI_IDLE_BYTE;

    case SD_SPI_BUSY:
        if (s->busy_cycles > 0) {
            s->busy_cycles--;
            return 0x00;
        }
        s->state = SD_SPI_IDLE;
        return SD_SPI_IDLE_BYTE;
    }

    return SD_SPI_IDLE_BYTE;
}

void tdeck_sd_spi_set_inserted(TdeckSdSpiState *s, bool inserted)
{
    s->inserted = inserted;
    if (!inserted) {
        reset_state(s);
    }
    DPRINTF("Card %s\n", inserted ? "inserted" : "ejected");
}

/* QOM boilerplate */

static void tdeck_sd_spi_reset(DeviceState *dev)
{
    TdeckSdSpiState *s = TDECK_SD_SPI(dev);
    reset_state(s);
    s->inserted = false;
}

static void tdeck_sd_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, tdeck_sd_spi_reset);
    dc->user_creatable = false;
}

static const TypeInfo tdeck_sd_spi_types[] = {
    {
        .name          = TYPE_TDECK_SD_SPI,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(TdeckSdSpiState),
        .class_init    = tdeck_sd_spi_class_init,
    },
};

DEFINE_TYPES(tdeck_sd_spi_types)
