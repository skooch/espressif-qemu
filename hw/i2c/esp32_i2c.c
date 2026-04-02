#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/i2c/esp32_i2c.h"
#include "hw/irq.h"

static void esp32_i2c_do_transaction(Esp32I2CState * s);
static void esp32_i2c_update_irq(Esp32I2CState * s);

/* Deferred transaction completion callback.
 * Sets INT_RAW bits and fires the IRQ after a brief delay, so the
 * firmware's async I2C future returns Pending on first poll and the
 * executor can run other tasks before the transaction "completes". */
static void esp32_i2c_completion_timer_cb(void *opaque)
{
    Esp32I2CState *s = Esp32_I2C(opaque);
    I2C_REG(s, A_I2C_INT_RAW) |= s->deferred_int_raw;
    s->deferred_int_raw = 0;
    esp32_i2c_update_irq(s);
}

/* Deferred IRQ callback */
static void esp32_i2c_irq_timer_cb(void *opaque)
{
    Esp32I2CState *s = Esp32_I2C(opaque);
    qemu_set_irq(s->irq, s->pending_irq);
}

static void esp32_i2c_reset_hold(Object *obj, ResetType type)
{
    Esp32I2CState * s = Esp32_I2C(obj);

    fifo8_reset(&s->rx_fifo);
    fifo8_reset(&s->tx_fifo);
    s->trans_ongoing = false;
    memset(s->regs, 0, sizeof(s->regs));
}

static uint32_t esp32_i2c_get_status_reg(Esp32I2CState* s)
{
    uint32_t res = 0;
    /* Bit 0 (RESP_REC): 1 = ACK received from slave.
     * Set when not in an active transaction (completed successfully). */
    if (!s->trans_ongoing) {
        res |= 1; /* RESP_REC = 1 */
    }
    res = FIELD_DP32(res, I2C_STATUS, BUS_BUSY, s->trans_ongoing);
    res = FIELD_DP32(res, I2C_STATUS, RXFIFO_CNT, fifo8_num_used(&s->rx_fifo));
    res = FIELD_DP32(res, I2C_STATUS, TXFIFO_CNT, fifo8_num_used(&s->tx_fifo));
    return res;
}

static void esp32_i2c_update_irq(Esp32I2CState * s)
{
    /* Defer IRQ to prevent re-entrant interrupt processing.
     *
     * When the firmware writes INT_ENA from I2cFuture::new(), a synchronous
     * qemu_set_irq causes the interrupt dispatcher to run INSIDE the MMIO
     * write. This triggers the GPIO interrupt handler (also active), which
     * tries to acquire a lock already held by the current context — deadlock.
     *
     * Deferring with timer_mod_ns(+0) schedules the IRQ delivery for after
     * the current translation block exits, preventing re-entrancy.
     * Deassertion is immediate since it never causes re-entrancy issues. */
    int irq_state = !!(I2C_REG(s, A_I2C_INT_RAW) & I2C_REG(s, A_I2C_INT_ENA));
    if (irq_state) {
        s->pending_irq = 1;
        /* +1000ns (1us) ensures the timer fires in a new TB */
        timer_mod_ns(s->irq_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
    } else {
        s->pending_irq = 0;
        timer_del(s->irq_timer);
        qemu_set_irq(s->irq, 0);
    }
}

static uint64_t esp32_i2c_read(void * opaque, hwaddr addr, unsigned int size)
{
    Esp32I2CState * s = Esp32_I2C(opaque);

    switch(addr) {
    case A_I2C_STATUS:
        return esp32_i2c_get_status_reg(s);
    case A_I2C_FIFO_DATA: {
        if (fifo8_num_used(&s->rx_fifo) == 0) {
            error_report("esp32_i2c: read I2C FIFO while it is empty");
            return 0xee;
        }
        return fifo8_pop(&s->rx_fifo);
    }
    case A_I2C_INT_ST:
        return I2C_REG(s, A_I2C_INT_RAW) & I2C_REG(s, A_I2C_INT_ENA);
    default:
        /* All other registers: return stored value */
        if (addr / 4 < ESP32_I2C_REG_COUNT) {
            uint32_t val = s->regs[addr / 4];
            /* Force DONE bit on CMD register reads. The firmware polls
             * command_done() in all_commands_done() which busy-spins if
             * any executed command lacks the DONE bit. In QEMU, commands
             * complete synchronously and DONE is set, but stale values
             * from interleaved or previous transactions can lack it. */
            if (addr >= A_I2C_CMD &&
                addr < A_I2C_CMD + ESP32_I2C_CMD_COUNT * 4) {
                val |= R_I2C_CMD_DONE_MASK;
            }
            return val;
        }
        return 0;
    }
}

static void esp32_i2c_write(void * opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    Esp32I2CState * s = Esp32_I2C(opaque);

    /* Store all writes to the register array for generic read-back */
    if (addr / 4 < ESP32_I2C_REG_COUNT) {
        s->regs[addr / 4] = (uint32_t)value;
    }

    /* Special handling for specific registers */
    switch(addr) {
    case A_I2C_CTR:
        if (FIELD_EX32(value, I2C_CTR, MS_MODE) != 1) {
            error_report("esp32_i2c: slave mode not implemented");
        }
        if (FIELD_EX32(value, I2C_CTR, TRANS_START)) {
            esp32_i2c_do_transaction(s);
            /* Auto-clear WT bits: TRANS_START, CONF_UPGATE */
            value &= ~(R_I2C_CTR_TRANS_START_MASK | R_I2C_CTR_CONF_UPGATE_MASK);
            s->regs[addr / 4] = (uint32_t)value;
        } else {
            /* Auto-clear CONF_UPGATE even without TRANS_START */
            value &= ~R_I2C_CTR_CONF_UPGATE_MASK;
            s->regs[addr / 4] = (uint32_t)value;
        }
        break;
    case A_I2C_FIFO_CONF:
        if (FIELD_EX32(value, I2C_FIFO_CONF, NONFIFO_EN)) {
            error_report("esp32_i2c: APB mode not implemented");
        }
        if (FIELD_EX32(value, I2C_FIFO_CONF, RX_FIFO_RST)) {
            fifo8_reset(&s->rx_fifo);
        }
        if (FIELD_EX32(value, I2C_FIFO_CONF, TX_FIFO_RST)) {
            fifo8_reset(&s->tx_fifo);
        }
        /* Auto-clear reset bits */
        value &= ~(R_I2C_FIFO_CONF_RX_FIFO_RST_MASK | R_I2C_FIFO_CONF_TX_FIFO_RST_MASK);
        s->regs[addr / 4] = (uint32_t)value;
        break;
    case A_I2C_FIFO_DATA:
        if (fifo8_num_free(&s->tx_fifo) == 0) {
            error_report("esp32_i2c: write to I2C TX FIFO while it is full");
        } else {
            fifo8_push(&s->tx_fifo, value);
        }
        break;
    case A_I2C_INT_CLR:
        I2C_REG(s, A_I2C_INT_RAW) &= ~(uint32_t)value;
        esp32_i2c_update_irq(s);
        break;
    case A_I2C_INT_ENA:
        esp32_i2c_update_irq(s);
        break;
    default:
        /* Value already stored above */
        break;
    }
}

static void esp32_i2c_do_transaction(Esp32I2CState * s)
{
    bool stop_or_end = false;
    for (int i_cmd = 0; i_cmd < ESP32_I2C_CMD_COUNT && !stop_or_end; ++i_cmd) {
        uint32_t cmd = s->regs[(A_I2C_CMD / 4) + i_cmd];
        int opcode = FIELD_EX32(cmd, I2C_CMD, OPCODE);

        /* Normalize ESP32-S3 opcodes to ESP32 numbering */
        if (opcode == 6) {
            opcode = I2C_OPCODE_RSTART;
        } else if (opcode == 2) {
            if (FIELD_EX32(cmd, I2C_CMD, BYTE_NUM) == 0) {
                opcode = I2C_OPCODE_STOP;
            }
        } else if (opcode == 3) {
            if (FIELD_EX32(cmd, I2C_CMD, BYTE_NUM) > 0) {
                opcode = I2C_OPCODE_READ;
            }
        }

        switch (opcode) {
            case I2C_OPCODE_RSTART:
                i2c_end_transfer(s->bus);
                s->trans_ongoing = false;
                break;
            case I2C_OPCODE_WRITE: {
                size_t length = FIELD_EX32(cmd, I2C_CMD, BYTE_NUM);
                if (!s->trans_ongoing) {
                    s->trans_ongoing = true;
                    uint8_t data = fifo8_pop(&s->tx_fifo);
                    uint8_t addr = data >> 1;
                    uint8_t is_read = data & 0x1;
                    int xfer_result = i2c_start_transfer(s->bus, addr, is_read);
                    if (xfer_result != 0) {
                        /* NACK */
                        if (FIELD_EX32(cmd, I2C_CMD, ACK_CHECK_EN)
                            && FIELD_EX32(cmd, I2C_CMD, ACK_EXP) == 0) {
                            I2C_REG(s, A_I2C_INT_RAW) = FIELD_DP32(
                                I2C_REG(s, A_I2C_INT_RAW), I2C_INT_RAW, ACK_ERR, 1);
                            stop_or_end = true;
                        }
                        s->trans_ongoing = false;
                        break;
                    }
                    I2C_REG(s, A_I2C_INT_RAW) = FIELD_DP32(
                        I2C_REG(s, A_I2C_INT_RAW), I2C_INT_RAW, ACK_ERR, 0);
                    length -= 1;
                }
                for (size_t nbytes = 0; nbytes < length; ++nbytes) {
                    uint8_t data = fifo8_pop(&s->tx_fifo);
                    i2c_send(s->bus, data);
                }
                break;
            }
            case I2C_OPCODE_READ: {
                size_t length = FIELD_EX32(cmd, I2C_CMD, BYTE_NUM);
                for (size_t nbytes = 0; nbytes < length; ++nbytes) {
                    if (fifo8_num_free(&s->rx_fifo) == 0) {
                        error_report("esp32_i2c: RX FIFO overflow");
                    } else {
                        uint8_t data = i2c_recv(s->bus);
                        fifo8_push(&s->rx_fifo, data);
                    }
                }
                break;
            }
            case I2C_OPCODE_STOP:
                i2c_end_transfer(s->bus);
                s->trans_ongoing = false;
                /* Defer TRANS_COMPLETE so the async future yields before completion */
                s->deferred_int_raw |= R_I2C_INT_RAW_TRANS_COMPLETE_MASK;
                stop_or_end = true;
                break;
            case I2C_OPCODE_END:
                /* Defer END_DETECT so the async future yields before completion */
                s->deferred_int_raw |= R_I2C_INT_RAW_END_DETECT_MASK;
                stop_or_end = true;
                break;
            default:
                error_report("esp32_i2c: Invalid command %d opcode %d", i_cmd, opcode);
                break;
        }
        s->regs[(A_I2C_CMD / 4) + i_cmd] = FIELD_DP32(
            s->regs[(A_I2C_CMD / 4) + i_cmd], I2C_CMD, DONE, 1);
    }

    /* Schedule deferred completion: set INT_RAW bits after 2us.
     * This ensures the async I2C future returns Pending on first poll,
     * giving the embassy executor a chance to run other tasks. */
    if (s->deferred_int_raw) {
        timer_mod_ns(s->completion_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2000);
    }
}

static const MemoryRegionOps esp32_i2c_ops = {
    .read = esp32_i2c_read,
    .write = esp32_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_i2c_init(Object * obj)
{
    Esp32I2CState *s = Esp32_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32_i2c_ops, s, TYPE_ESP32_I2C, ESP32_I2C_MEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->bus = i2c_init_bus(DEVICE(s), "i2c");

    fifo8_create(&s->tx_fifo, ESP32_I2C_FIFO_LENGTH);
    fifo8_create(&s->rx_fifo, ESP32_I2C_FIFO_LENGTH);

    s->irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp32_i2c_irq_timer_cb, s);
    s->completion_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, esp32_i2c_completion_timer_cb, s);
    s->deferred_int_raw = 0;
}

static void esp32_i2c_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.hold = esp32_i2c_reset_hold;
}

static const TypeInfo esp32_i2c_type_info = {
    .name = TYPE_ESP32_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32I2CState),
    .instance_init = esp32_i2c_init,
    .class_init = esp32_i2c_class_init
};

static void esp32_i2c_register_types(void)
{
    type_register_static(&esp32_i2c_type_info);
}

type_init(esp32_i2c_register_types)
