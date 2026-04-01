/*
 * T-Deck Pro SX1262 LoRa SPI slave model
 *
 * Emulates SX1262 SPI command protocol for QEMU simulation.
 * Supports init sequence, TX/RX with loopback, IRQ via DIO1/BUSY GPIOs.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 * License: GPLv2+
 */

#include "qemu/osdep.h"
#include "hw/ssi/tdeck_lora_sx1262.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "qapi/error.h"

/* #define DEBUG_TDECK_LORA 1 */

#ifdef DEBUG_TDECK_LORA
#define DPRINTF(fmt, ...) \
    qemu_log("tdeck-lora-sx1262: " fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

/* SX1262 opcodes */
#define OP_SET_SLEEP            0x84
#define OP_SET_STANDBY          0x80
#define OP_SET_TX               0x83
#define OP_SET_RX               0x82
#define OP_SET_REGULATOR_MODE   0x96
#define OP_CALIBRATE            0x89
#define OP_CALIBRATE_IMAGE      0x98
#define OP_SET_DIO_IRQ_PARAMS   0x08
#define OP_SET_DIO2_RF_SWITCH   0x9D
#define OP_SET_DIO3_TCXO        0x97
#define OP_SET_RF_FREQUENCY     0x86
#define OP_SET_PACKET_TYPE      0x8A
#define OP_SET_TX_PARAMS        0x8E
#define OP_SET_MOD_PARAMS       0x8B
#define OP_SET_PACKET_PARAMS    0x8C
#define OP_SET_BUFFER_BASE_ADDR 0x8F
#define OP_SET_PA_CONFIG        0x95
#define OP_WRITE_BUFFER         0x0E
#define OP_READ_BUFFER          0x1E
#define OP_GET_STATUS           0xC0
#define OP_GET_IRQ_STATUS       0x12
#define OP_CLEAR_IRQ_STATUS     0x02
#define OP_GET_RX_BUF_STATUS    0x13
#define OP_GET_PACKET_STATUS    0x14
#define OP_GET_RSSI_INST        0x15
#define OP_GET_DEVICE_ERRORS    0x17
#define OP_CLEAR_DEVICE_ERRORS  0x07
#define OP_WRITE_REGISTER       0x0D
#define OP_READ_REGISTER        0x1D

/* IRQ bits */
#define IRQ_TX_DONE  0x0001
#define IRQ_RX_DONE  0x0002

/* Timer delays */
#define TX_DONE_DELAY_NS   (50 * NANOSECONDS_PER_SECOND / 1000)
#define RX_LOOPBACK_DELAY_NS (10 * NANOSECONDS_PER_SECOND / 1000)
#define BUSY_DEASSERT_DELAY_NS (100 * NANOSECONDS_PER_SECOND / 1000000)

static uint8_t status_byte(TdeckLoraSx1262State *s)
{
    return (s->chip_mode << 4) | (s->cmd_status << 1);
}

static void pulse_busy(TdeckLoraSx1262State *s)
{
    qemu_set_irq(s->busy, 1);
    timer_mod(s->busy_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BUSY_DEASSERT_DELAY_NS);
}

static void update_dio1(TdeckLoraSx1262State *s)
{
    if (s->irq_status & s->dio1_mask) {
        qemu_set_irq(s->dio1, 1);
    } else {
        qemu_set_irq(s->dio1, 0);
    }
}

/*
 * Look up expected parameter count for an opcode.
 * Returns -1 for variable-length commands (WriteBuffer, ReadBuffer,
 * WriteRegister, ReadRegister) which are handled specially.
 */
static int opcode_param_count(uint8_t opcode)
{
    switch (opcode) {
    case OP_SET_SLEEP:          return 1;
    case OP_SET_STANDBY:        return 1;
    case OP_SET_TX:             return 3;
    case OP_SET_RX:             return 3;
    case OP_SET_REGULATOR_MODE: return 1;
    case OP_CALIBRATE:          return 1;
    case OP_CALIBRATE_IMAGE:    return 2;
    case OP_SET_DIO_IRQ_PARAMS: return 8;
    case OP_SET_DIO2_RF_SWITCH: return 1;
    case OP_SET_DIO3_TCXO:      return 4;
    case OP_SET_RF_FREQUENCY:   return 4;
    case OP_SET_PACKET_TYPE:    return 1;
    case OP_SET_TX_PARAMS:      return 2;
    case OP_SET_MOD_PARAMS:     return 4;
    case OP_SET_PACKET_PARAMS:  return 6;
    case OP_SET_BUFFER_BASE_ADDR: return 2;
    case OP_SET_PA_CONFIG:      return 4;
    case OP_CLEAR_IRQ_STATUS:   return 2;
    case OP_CLEAR_DEVICE_ERRORS: return 2;
    /* Read commands: NOP param(s) then response bytes */
    case OP_GET_STATUS:         return 1;
    case OP_GET_IRQ_STATUS:     return 1;
    case OP_GET_RX_BUF_STATUS:  return 1;
    case OP_GET_PACKET_STATUS:  return 1;
    case OP_GET_RSSI_INST:      return 1;
    case OP_GET_DEVICE_ERRORS:  return 1;
    /* Variable-length */
    case OP_WRITE_BUFFER:       return -1;
    case OP_READ_BUFFER:        return -1;
    case OP_WRITE_REGISTER:     return -1;
    case OP_READ_REGISTER:      return -1;
    default:                    return 0;
    }
}

/*
 * Execute a completed command (all params received).
 * Prepares resp_buf/resp_len for read commands.
 */
static void execute_command(TdeckLoraSx1262State *s)
{
    DPRINTF("exec opcode=0x%02x params=%d\n", s->opcode, s->param_idx);

    switch (s->opcode) {
    case OP_SET_SLEEP:
        s->chip_mode = SX1262_MODE_SLEEP;
        break;

    case OP_SET_STANDBY:
        s->chip_mode = (s->param_buf[0] == 1) ?
            SX1262_MODE_STDBY_XOSC : SX1262_MODE_STDBY_RC;
        pulse_busy(s);
        break;

    case OP_SET_TX:
        s->chip_mode = SX1262_MODE_TX;
        /* Copy tx data for loopback */
        s->last_tx_len = s->payload_len;
        memcpy(s->last_tx_data, &s->buffer[s->tx_base], s->payload_len);
        /* Schedule TX done */
        timer_mod(s->tx_done_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TX_DONE_DELAY_NS);
        qemu_set_irq(s->busy, 1);
        break;

    case OP_SET_RX:
        s->chip_mode = SX1262_MODE_RX;
        pulse_busy(s);
        break;

    case OP_SET_REGULATOR_MODE:
        s->regulator_mode = s->param_buf[0];
        pulse_busy(s);
        break;

    case OP_CALIBRATE:
        pulse_busy(s);
        break;

    case OP_CALIBRATE_IMAGE:
        pulse_busy(s);
        break;

    case OP_SET_DIO_IRQ_PARAMS:
        s->irq_mask = ((uint16_t)s->param_buf[0] << 8) | s->param_buf[1];
        s->dio1_mask = ((uint16_t)s->param_buf[2] << 8) | s->param_buf[3];
        /* dio2_mask and dio3_mask ignored */
        break;

    case OP_SET_DIO2_RF_SWITCH:
        s->dio2_rf_switch = s->param_buf[0] != 0;
        break;

    case OP_SET_DIO3_TCXO:
        s->dio3_tcxo = true;
        break;

    case OP_SET_RF_FREQUENCY:
        s->frequency_hz = ((uint32_t)s->param_buf[0] << 24) |
                          ((uint32_t)s->param_buf[1] << 16) |
                          ((uint32_t)s->param_buf[2] << 8)  |
                          s->param_buf[3];
        break;

    case OP_SET_PACKET_TYPE:
        s->packet_type = s->param_buf[0];
        break;

    case OP_SET_TX_PARAMS:
        s->tx_power = (int8_t)s->param_buf[0];
        break;

    case OP_SET_MOD_PARAMS:
        s->sf = s->param_buf[0];
        s->bw = s->param_buf[1];
        s->cr = s->param_buf[2];
        break;

    case OP_SET_PACKET_PARAMS:
        s->preamble_len_hi = s->param_buf[0];
        s->preamble_len_lo = s->param_buf[1];
        /* param_buf[2] = header type, [3] = payload_len, [4] = crc, [5] = iq */
        s->payload_len = s->param_buf[3];
        break;

    case OP_SET_BUFFER_BASE_ADDR:
        s->tx_base = s->param_buf[0];
        s->rx_base = s->param_buf[1];
        break;

    case OP_SET_PA_CONFIG:
        /* Store but no-op */
        break;

    case OP_GET_STATUS:
        /* Status already returned as first byte; no additional response */
        break;

    case OP_GET_IRQ_STATUS:
        s->resp_buf[0] = (s->irq_status >> 8) & 0xFF;
        s->resp_buf[1] = s->irq_status & 0xFF;
        s->resp_len = 2;
        s->resp_idx = 0;
        break;

    case OP_CLEAR_IRQ_STATUS: {
        uint16_t mask = ((uint16_t)s->param_buf[0] << 8) | s->param_buf[1];
        s->irq_status &= ~mask;
        update_dio1(s);
        if (s->cmd_status == 2 && !(s->irq_status & IRQ_RX_DONE)) {
            s->cmd_status = 0;
        }
        break;
    }

    case OP_GET_RX_BUF_STATUS:
        s->resp_buf[0] = s->payload_len;
        s->resp_buf[1] = s->rx_base;
        s->resp_len = 2;
        s->resp_idx = 0;
        break;

    case OP_GET_PACKET_STATUS:
        s->resp_buf[0] = 120; /* RSSI = -60 dBm (value/(-2)) */
        s->resp_buf[1] = 40;  /* SNR = 40/4 = 10 dB */
        s->resp_buf[2] = 120; /* Signal RSSI */
        s->resp_len = 3;
        s->resp_idx = 0;
        break;

    case OP_GET_RSSI_INST:
        s->resp_buf[0] = 120; /* -60 dBm */
        s->resp_len = 1;
        s->resp_idx = 0;
        break;

    case OP_GET_DEVICE_ERRORS:
        s->resp_buf[0] = 0;
        s->resp_buf[1] = 0;
        s->resp_len = 2;
        s->resp_idx = 0;
        break;

    case OP_CLEAR_DEVICE_ERRORS:
        break;

    default:
        DPRINTF("unhandled opcode 0x%02x\n", s->opcode);
        break;
    }
}

/* Timer callbacks */

static void tx_done_cb(void *opaque)
{
    TdeckLoraSx1262State *s = opaque;

    DPRINTF("TX done\n");
    s->irq_status |= IRQ_TX_DONE;
    s->cmd_status = 6; /* CmdTxDone */
    s->chip_mode = SX1262_MODE_STDBY_RC;
    qemu_set_irq(s->busy, 0);
    update_dio1(s);

    if (s->loopback_enabled && s->last_tx_len > 0) {
        /* Copy TX data to RX buffer for loopback */
        memcpy(&s->buffer[s->rx_base], s->last_tx_data, s->last_tx_len);
        s->payload_len = s->last_tx_len;
        timer_mod(s->rx_loopback_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RX_LOOPBACK_DELAY_NS);
    }
}

static void rx_loopback_cb(void *opaque)
{
    TdeckLoraSx1262State *s = opaque;

    DPRINTF("RX loopback done\n");
    s->irq_status |= IRQ_RX_DONE;
    s->cmd_status = 2; /* DataAvailable */
    update_dio1(s);
}

static void busy_deassert_cb(void *opaque)
{
    TdeckLoraSx1262State *s = opaque;
    qemu_set_irq(s->busy, 0);
}

/* Public API */

void tdeck_lora_reset(TdeckLoraSx1262State *s)
{
    s->chip_mode = SX1262_MODE_STDBY_RC;
    s->cmd_status = 0;
    s->packet_type = 1; /* LoRa */
    s->regulator_mode = 0;
    s->frequency_hz = 0;
    s->tx_power = 0;
    s->sf = 7;
    s->bw = 0;
    s->cr = 1;
    s->sync_word = 0;
    s->irq_mask = 0;
    s->dio1_mask = 0;
    s->dio2_rf_switch = false;
    s->dio3_tcxo = false;
    s->boosted_rx = false;
    s->tx_base = 0;
    s->rx_base = 0;
    s->payload_len = 0;
    s->preamble_len_hi = 0;
    s->preamble_len_lo = 0;
    s->last_tx_len = 0;
    s->irq_status = 0;
    s->loopback_enabled = true;

    s->spi_state = SX1262_SPI_IDLE;
    s->opcode = 0;
    s->param_idx = 0;
    s->param_expected = 0;
    s->resp_idx = 0;
    s->resp_len = 0;
    s->data_offset = 0;
    s->data_count = 0;
    s->in_write_buffer = false;
    s->in_read_buffer = false;

    memset(s->buffer, 0, sizeof(s->buffer));
    memset(s->last_tx_data, 0, sizeof(s->last_tx_data));

    qemu_set_irq(s->busy, 0);
    qemu_set_irq(s->dio1, 0);

    timer_del(s->tx_done_timer);
    timer_del(s->rx_loopback_timer);
    timer_del(s->busy_timer);

    DPRINTF("reset\n");
}

uint8_t tdeck_lora_spi_transfer(TdeckLoraSx1262State *s, uint8_t mosi)
{
    uint8_t miso = status_byte(s);

    switch (s->spi_state) {
    case SX1262_SPI_IDLE:
        /* First byte after CS assert = opcode */
        s->spi_state = SX1262_SPI_OPCODE;
        /* Fall through to process as opcode */
        /* fallthrough */

    case SX1262_SPI_OPCODE: {
        s->opcode = mosi;
        s->param_idx = 0;
        s->resp_len = 0;
        s->resp_idx = 0;
        s->in_write_buffer = false;
        s->in_read_buffer = false;

        int nparams = opcode_param_count(mosi);

        if (nparams < 0) {
            /* Variable-length commands */
            switch (mosi) {
            case OP_WRITE_BUFFER:
                /* Next byte is offset, then variable data */
                s->param_expected = 1;
                s->spi_state = SX1262_SPI_PARAMS;
                s->in_write_buffer = true;
                break;
            case OP_READ_BUFFER:
                /* Next byte is offset, then NOP, then data out */
                s->param_expected = 2;
                s->spi_state = SX1262_SPI_PARAMS;
                s->in_read_buffer = true;
                break;
            case OP_WRITE_REGISTER:
                /* 2 addr bytes, then variable data (ignored) */
                s->param_expected = 2;
                s->spi_state = SX1262_SPI_PARAMS;
                break;
            case OP_READ_REGISTER:
                /* 2 addr bytes + 1 NOP, then data out (return 0) */
                s->param_expected = 3;
                s->spi_state = SX1262_SPI_PARAMS;
                break;
            default:
                s->spi_state = SX1262_SPI_IDLE;
                break;
            }
        } else if (nparams == 0) {
            execute_command(s);
            if (s->resp_len > 0) {
                s->spi_state = SX1262_SPI_RESPONSE;
            } else {
                s->spi_state = SX1262_SPI_IDLE;
            }
        } else {
            s->param_expected = nparams;
            s->spi_state = SX1262_SPI_PARAMS;
        }

        DPRINTF("opcode 0x%02x nparams=%d\n", mosi, nparams);
        return miso;
    }

    case SX1262_SPI_PARAMS:
        if (s->param_idx < s->param_expected &&
            s->param_idx < sizeof(s->param_buf)) {
            s->param_buf[s->param_idx++] = mosi;
        }

        if (s->param_idx >= s->param_expected) {
            if (s->in_write_buffer) {
                /* First param was the offset */
                s->data_offset = s->param_buf[0];
                s->spi_state = SX1262_SPI_DATA;
                DPRINTF("WriteBuffer offset=%d\n", s->data_offset);
            } else if (s->in_read_buffer) {
                /* First param was offset, second was NOP */
                s->data_offset = s->param_buf[0];
                s->spi_state = SX1262_SPI_DATA;
                DPRINTF("ReadBuffer offset=%d\n", s->data_offset);
            } else if (s->opcode == OP_WRITE_REGISTER) {
                /* Addr received, rest is data (ignored) */
                s->spi_state = SX1262_SPI_DATA;
            } else if (s->opcode == OP_READ_REGISTER) {
                /* Addr + NOP received, return 0x00 for data */
                s->spi_state = SX1262_SPI_DATA;
            } else {
                execute_command(s);
                if (s->resp_len > 0) {
                    s->spi_state = SX1262_SPI_RESPONSE;
                } else {
                    s->spi_state = SX1262_SPI_IDLE;
                }
            }
        }
        return miso;

    case SX1262_SPI_DATA:
        if (s->in_write_buffer) {
            s->buffer[s->data_offset] = mosi;
            s->data_offset++;
            return miso;
        } else if (s->in_read_buffer) {
            uint8_t val = s->buffer[s->data_offset];
            s->data_offset++;
            return val;
        } else if (s->opcode == OP_READ_REGISTER) {
            return 0x00;
        }
        /* WriteRegister: consume and ignore */
        return miso;

    case SX1262_SPI_RESPONSE:
        if (s->resp_idx < s->resp_len) {
            return s->resp_buf[s->resp_idx++];
        }
        /* Response exhausted, return idle */
        return 0x00;
    }

    return miso;
}

/* QOM boilerplate */

static void tdeck_lora_sx1262_reset_handler(DeviceState *dev)
{
    TdeckLoraSx1262State *s = TDECK_LORA_SX1262(dev);
    tdeck_lora_reset(s);
}

static void tdeck_lora_sx1262_init(Object *obj)
{
    TdeckLoraSx1262State *s = TDECK_LORA_SX1262(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->dio1, "dio1", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->busy, "busy", 1);

    s->tx_done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tx_done_cb, s);
    s->rx_loopback_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rx_loopback_cb, s);
    s->busy_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, busy_deassert_cb, s);
}

static void tdeck_lora_sx1262_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, tdeck_lora_sx1262_reset_handler);
    dc->user_creatable = false;
}

static const TypeInfo tdeck_lora_sx1262_types[] = {
    {
        .name          = TYPE_TDECK_LORA_SX1262,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(TdeckLoraSx1262State),
        .instance_init = tdeck_lora_sx1262_init,
        .class_init    = tdeck_lora_sx1262_class_init,
    },
};

DEFINE_TYPES(tdeck_lora_sx1262_types)
