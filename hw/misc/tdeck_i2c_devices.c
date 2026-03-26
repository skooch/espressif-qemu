/*
 * T-Deck Pro I2C slave device models
 *
 * Simple register-file I2C slaves for battery monitoring and input devices.
 * Each device responds to reads/writes at its I2C address with sensible
 * default values.
 *
 * Devices:
 *   - BQ25896 charger (addr 0x6B): 21 registers, reports USB connected + charging
 *   - BQ27220 fuel gauge (addr 0x55): 16-bit LE registers, reports 75% SOC
 *   - TCA8418 keyboard (addr 0x34): register file + key event FIFO
 *   - CST328 touch (addr 0x1A): 28-byte touch data read
 *
 * Copyright (c) 2026 T-Deck Pro Project
 * License: GPLv2+
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"

/* ========================================================================= */
/* BQ25896 Charger                                                           */
/* ========================================================================= */

#define TYPE_TDECK_BQ25896 "tdeck-bq25896"
OBJECT_DECLARE_SIMPLE_TYPE(TdeckBq25896State, TDECK_BQ25896)

struct TdeckBq25896State {
    I2CSlave parent_obj;
    uint8_t regs[0x15];  /* 21 registers (0x00-0x14) */
    uint8_t reg_addr;
    bool addr_set;
};

static void tdeck_bq25896_reset(DeviceState *dev)
{
    TdeckBq25896State *s = TDECK_BQ25896(dev);
    memset(s->regs, 0, sizeof(s->regs));
    /* REG0B: Not charging, no USB input */
    s->regs[0x0B] = 0x00;
    /* REG0E: Battery voltage ADC ~ 3.8V (0x1E = offset from 2.304V base) */
    s->regs[0x0E] = 0x1E;
    /* REG14: Part number / revision */
    s->regs[0x14] = 0x23;
    s->reg_addr = 0;
    s->addr_set = false;
}

static int tdeck_bq25896_event(I2CSlave *i2c, enum i2c_event event)
{
    TdeckBq25896State *s = TDECK_BQ25896(i2c);
    if (event == I2C_START_SEND) {
        s->addr_set = false;
    }
    return 0;
}

static uint8_t tdeck_bq25896_recv(I2CSlave *i2c)
{
    TdeckBq25896State *s = TDECK_BQ25896(i2c);
    uint8_t val = 0;
    if (s->reg_addr < sizeof(s->regs)) {
        val = s->regs[s->reg_addr];
    }
    s->reg_addr++;
    return val;
}

static int tdeck_bq25896_send(I2CSlave *i2c, uint8_t data)
{
    TdeckBq25896State *s = TDECK_BQ25896(i2c);
    if (!s->addr_set) {
        s->reg_addr = data;
        s->addr_set = true;
    } else {
        if (s->reg_addr < sizeof(s->regs) && s->reg_addr != 0x14) {
            s->regs[s->reg_addr] = data;
        }
        s->reg_addr++;
    }
    return 0;
}

static void tdeck_bq25896_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    dc->legacy_reset = tdeck_bq25896_reset;
    sc->event = tdeck_bq25896_event;
    sc->recv = tdeck_bq25896_recv;
    sc->send = tdeck_bq25896_send;
}

static const TypeInfo tdeck_bq25896_info = {
    .name = TYPE_TDECK_BQ25896,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TdeckBq25896State),
    .class_init = tdeck_bq25896_class_init,
};

/* ========================================================================= */
/* BQ27220 Fuel Gauge                                                        */
/* ========================================================================= */

#define TYPE_TDECK_BQ27220 "tdeck-bq27220"
OBJECT_DECLARE_SIMPLE_TYPE(TdeckBq27220State, TDECK_BQ27220)

struct TdeckBq27220State {
    I2CSlave parent_obj;
    uint8_t regs[0x40];  /* 64 bytes of register space (16-bit LE) */
    uint8_t reg_addr;
    bool addr_set;
    uint16_t ctrl_result; /* Result of last Control subcmd */
};

static void tdeck_bq27220_reset(DeviceState *dev)
{
    TdeckBq27220State *s = TDECK_BQ27220(dev);
    memset(s->regs, 0, sizeof(s->regs));
    /* Voltage: 3800mV (16-bit LE at 0x08) */
    s->regs[0x08] = 3800 & 0xFF;
    s->regs[0x09] = (3800 >> 8) & 0xFF;
    /* Current: 0mA (0x0C) */
    /* StateOfCharge: 75% (0x1C) */
    s->regs[0x1C] = 75;
    s->regs[0x1D] = 0;
    /* DesignCapacity: 3000mAh (0x3C) */
    s->regs[0x3C] = 3000 & 0xFF;
    s->regs[0x3D] = (3000 >> 8) & 0xFF;
    s->ctrl_result = 0;
    s->reg_addr = 0;
    s->addr_set = false;
}

static int tdeck_bq27220_event(I2CSlave *i2c, enum i2c_event event)
{
    TdeckBq27220State *s = TDECK_BQ27220(i2c);
    if (event == I2C_START_SEND) {
        s->addr_set = false;
    }
    return 0;
}

static uint8_t tdeck_bq27220_recv(I2CSlave *i2c)
{
    TdeckBq27220State *s = TDECK_BQ27220(i2c);
    uint8_t val = 0;
    if (s->reg_addr == 0x00) {
        /* Control register: return subcmd result (low byte) */
        val = s->ctrl_result & 0xFF;
        s->reg_addr++;
    } else if (s->reg_addr == 0x01) {
        /* Control register high byte */
        val = (s->ctrl_result >> 8) & 0xFF;
        s->reg_addr++;
    } else if (s->reg_addr < sizeof(s->regs)) {
        val = s->regs[s->reg_addr];
        s->reg_addr++;
    }
    return val;
}

static int tdeck_bq27220_send(I2CSlave *i2c, uint8_t data)
{
    TdeckBq27220State *s = TDECK_BQ27220(i2c);
    if (!s->addr_set) {
        s->reg_addr = data;
        s->addr_set = true;
    } else {
        if (s->reg_addr == 0x00) {
            /* Control subcmd low byte — store for 2-byte subcmd */
            s->ctrl_result = data;
        } else if (s->reg_addr == 0x01) {
            /* Control subcmd high byte */
            uint16_t subcmd = s->ctrl_result | ((uint16_t)data << 8);
            switch (subcmd) {
            case 0x0001: s->ctrl_result = 0x0220; break; /* DEVICE_NUMBER */
            case 0x0002: s->ctrl_result = 0x0109; break; /* FW_VERSION */
            default:     s->ctrl_result = 0x0000; break;
            }
        } else if (s->reg_addr < sizeof(s->regs)) {
            s->regs[s->reg_addr] = data;
        }
        s->reg_addr++;
    }
    return 0;
}

static void tdeck_bq27220_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    dc->legacy_reset = tdeck_bq27220_reset;
    sc->event = tdeck_bq27220_event;
    sc->recv = tdeck_bq27220_recv;
    sc->send = tdeck_bq27220_send;
}

static const TypeInfo tdeck_bq27220_info = {
    .name = TYPE_TDECK_BQ27220,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TdeckBq27220State),
    .class_init = tdeck_bq27220_class_init,
};

/* ========================================================================= */
/* TCA8418 Keyboard Controller                                               */
/* ========================================================================= */

#define TYPE_TDECK_TCA8418 "tdeck-tca8418"
OBJECT_DECLARE_SIMPLE_TYPE(TdeckTca8418State, TDECK_TCA8418)

#define TCA8418_REG_COUNT    0x40
#define TCA8418_FIFO_SIZE    10

struct TdeckTca8418State {
    I2CSlave parent_obj;
    uint8_t regs[TCA8418_REG_COUNT];
    uint8_t reg_addr;
    bool addr_set;
    /* Key event FIFO */
    uint8_t fifo[TCA8418_FIFO_SIZE];
    uint8_t fifo_count;
    uint8_t fifo_head;
    uint8_t fifo_tail;
    /* INT pin output (active LOW) */
    qemu_irq int_pin;
};

#define TCA8418_REG_CFG       0x01
#define TCA8418_REG_INT_STAT  0x02
#define TCA8418_REG_KEY_LCK   0x03
#define TCA8418_REG_KEY_EVENT 0x04

static void tdeck_tca8418_update_int(TdeckTca8418State *s)
{
    /* INT pin is active LOW when FIFO has events */
    bool active = s->fifo_count > 0;
    if (active) {
        s->regs[TCA8418_REG_INT_STAT] |= 0x01; /* K_INT */
    }
    qemu_set_irq(s->int_pin, active ? 0 : 1);
}

/*
 * Push a key event into the TCA8418 FIFO.
 * raw_code: TCA8418 key code (row*10 + col + 1)
 * pressed: true for press, false for release
 */
void tdeck_tca8418_inject_key(TdeckTca8418State *s, uint8_t raw_code, bool pressed)
{
    if (s->fifo_count >= TCA8418_FIFO_SIZE) {
        return; /* FIFO full, drop event */
    }
    uint8_t event = raw_code & 0x7F;
    if (pressed) {
        event |= 0x80;
    }
    s->fifo[s->fifo_tail] = event;
    s->fifo_tail = (s->fifo_tail + 1) % TCA8418_FIFO_SIZE;
    s->fifo_count++;
    tdeck_tca8418_update_int(s);
}

/*
 * Map an ASCII character to a TCA8418 raw key code.
 * The T-Deck keyboard has a 4x10 matrix with columns reversed.
 * Raw code = row*10 + (9-col) + 1.
 *
 * Layout (physical):
 *   Row 0: q w e r t y u i o p
 *   Row 1: a s d f g h j k l BSP
 *   Row 2: ALT z x c v b n m $ ENT
 *   Row 3: SHF MIC SPACE SPACE SPACE SPACE SPACE SYM SHF
 */
static uint8_t ascii_to_tca8418(char c)
{
    /* Row 0 */
    static const char row0[] = "qwertyuiop";
    /* Row 1 */
    static const char row1[] = "asdfghjkl";
    /* Row 2: position 0 is ALT (modifier, skipped by loop starting at i=1) */
    static const char row2[] = " zxcvbnm";

    for (int i = 0; i < 10; i++) {
        if (row0[i] == c) return (0 * 10) + (9 - i) + 1;
    }
    for (int i = 0; i < 9; i++) {
        if (row1[i] == c) return (1 * 10) + (9 - i) + 1;
    }
    if (c == '\b' || c == 127) return (1 * 10) + (9 - 9) + 1; /* BSP at row1 col9 */
    for (int i = 1; i < 8; i++) {
        if (row2[i] == c) return (2 * 10) + (9 - i) + 1;
    }
    if (c == '\r' || c == '\n') return (2 * 10) + (9 - 9) + 1; /* ENT at row2 col9 */
    if (c == ' ') return (3 * 10) + (9 - 2) + 1; /* SPACE at row3 col2 */

    /* Uppercase: same key with shift (not implemented here, just map to lowercase) */
    if (c >= 'A' && c <= 'Z') return ascii_to_tca8418(c - 'A' + 'a');

    return 0; /* Unknown key */
}

/*
 * Inject an ASCII character as a press+release pair.
 */
void tdeck_tca8418_inject_char(TdeckTca8418State *s, char c)
{
    uint8_t code = ascii_to_tca8418(c);
    if (code == 0) return;
    tdeck_tca8418_inject_key(s, code, true);
    tdeck_tca8418_inject_key(s, code, false);
}

static void tdeck_tca8418_reset(DeviceState *dev)
{
    TdeckTca8418State *s = TDECK_TCA8418(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->reg_addr = 0;
    s->addr_set = false;
    s->fifo_count = 0;
    s->fifo_head = 0;
    s->fifo_tail = 0;
    /* INT is active LOW; deassert (HIGH) when idle */
    qemu_set_irq(s->int_pin, 1);
}

static int tdeck_tca8418_event(I2CSlave *i2c, enum i2c_event event)
{
    TdeckTca8418State *s = TDECK_TCA8418(i2c);
    if (event == I2C_START_SEND) {
        s->addr_set = false;
    }
    return 0;
}

static uint8_t tdeck_tca8418_recv(I2CSlave *i2c)
{
    TdeckTca8418State *s = TDECK_TCA8418(i2c);
    uint8_t val = 0;

    switch (s->reg_addr) {
    case TCA8418_REG_INT_STAT:
        val = s->regs[TCA8418_REG_INT_STAT];
        break;
    case TCA8418_REG_KEY_LCK:
        /* Bits [3:0] = event count in FIFO */
        val = s->fifo_count & 0x0F;
        break;
    case TCA8418_REG_KEY_EVENT:
        /* Pop next event from FIFO */
        if (s->fifo_count > 0) {
            val = s->fifo[s->fifo_head];
            s->fifo_head = (s->fifo_head + 1) % TCA8418_FIFO_SIZE;
            s->fifo_count--;
            tdeck_tca8418_update_int(s);
        }
        break;
    default:
        if (s->reg_addr < TCA8418_REG_COUNT) {
            val = s->regs[s->reg_addr];
        }
        break;
    }
    return val;
}

static int tdeck_tca8418_send(I2CSlave *i2c, uint8_t data)
{
    TdeckTca8418State *s = TDECK_TCA8418(i2c);
    if (!s->addr_set) {
        s->reg_addr = data;
        s->addr_set = true;
    } else {
        if (s->reg_addr == TCA8418_REG_INT_STAT) {
            /* Write-1-to-clear */
            s->regs[TCA8418_REG_INT_STAT] &= ~data;
        } else if (s->reg_addr < TCA8418_REG_COUNT) {
            s->regs[s->reg_addr] = data;
        }
    }
    return 0;
}

static void tdeck_tca8418_realize(DeviceState *dev, Error **errp)
{
    TdeckTca8418State *s = TDECK_TCA8418(dev);
    /* INT pin output — active LOW when FIFO has events */
    qdev_init_gpio_out_named(dev, &s->int_pin, "int", 1);
}

static void tdeck_tca8418_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    dc->legacy_reset = tdeck_tca8418_reset;
    dc->realize = tdeck_tca8418_realize;
    sc->event = tdeck_tca8418_event;
    sc->recv = tdeck_tca8418_recv;
    sc->send = tdeck_tca8418_send;
}

static const TypeInfo tdeck_tca8418_info = {
    .name = TYPE_TDECK_TCA8418,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TdeckTca8418State),
    .class_init = tdeck_tca8418_class_init,
};

/* ========================================================================= */
/* CST328 Touch Controller                                                   */
/* ========================================================================= */

#define TYPE_TDECK_CST328 "tdeck-cst328"
OBJECT_DECLARE_SIMPLE_TYPE(TdeckCst328State, TDECK_CST328)

struct TdeckCst328State {
    I2CSlave parent_obj;
    uint8_t reg_addr;
    bool addr_set;
    uint8_t read_idx;
    /* Touch state */
    uint8_t finger_state;  /* 6 = pressed, 0 = released */
    uint16_t x;
    uint16_t y;
    /* INT pin (active LOW when touch data available) */
    qemu_irq int_pin;
};

/*
 * Inject a touch event. The CST328 fires INT LOW when touched and
 * keeps it LOW for repeated IRQ cycles until the firmware ACKs.
 * On release, finger_state goes to 0 and INT goes HIGH.
 */
void tdeck_cst328_inject_touch(TdeckCst328State *s,
                                uint16_t x, uint16_t y, bool pressed)
{
    s->x = x;
    s->y = y;
    s->finger_state = pressed ? 6 : 0;
    /* INT is active LOW when touch data is available */
    qemu_set_irq(s->int_pin, pressed ? 0 : 1);
}

static void tdeck_cst328_reset(DeviceState *dev)
{
    TdeckCst328State *s = TDECK_CST328(dev);
    s->reg_addr = 0;
    s->addr_set = false;
    s->read_idx = 0;
    s->finger_state = 0;
    s->x = 0;
    s->y = 0;
    /* INT is active LOW; deassert (HIGH) when idle */
    qemu_set_irq(s->int_pin, 1);
}

static int tdeck_cst328_event(I2CSlave *i2c, enum i2c_event event)
{
    TdeckCst328State *s = TDECK_CST328(i2c);
    if (event == I2C_START_SEND) {
        s->addr_set = false;
        s->read_idx = 0;
    }
    if (event == I2C_START_RECV) {
        s->read_idx = 0;
    }
    return 0;
}

static uint8_t tdeck_cst328_recv(I2CSlave *i2c)
{
    TdeckCst328State *s = TDECK_CST328(i2c);
    uint8_t val = 0;

    /* Touch data is 28 bytes starting from register 0x00 */
    switch (s->read_idx) {
    case 0: val = s->finger_state; break;              /* finger state */
    case 1: val = (s->x >> 4) & 0xFF; break;           /* x_hi */
    case 2: val = (s->y >> 4) & 0xFF; break;           /* y_hi */
    case 3: val = ((s->x & 0x0F) << 4) | (s->y & 0x0F); break; /* xy_lo */
    case 4: val = 0; break;                             /* reserved */
    case 5: val = (s->finger_state == 6) ? 1 : 0; break; /* touch count */
    case 6: val = 0xAB; break;                          /* sync byte */
    default: val = 0; break;                            /* padding */
    }
    s->read_idx++;
    return val;
}

static int tdeck_cst328_send(I2CSlave *i2c, uint8_t data)
{
    TdeckCst328State *s = TDECK_CST328(i2c);
    if (!s->addr_set) {
        s->reg_addr = data;
        s->addr_set = true;
    }
    /* ACK write (0xAB to reg 0x00) and debug mode writes are accepted silently */
    return 0;
}

static void tdeck_cst328_realize(DeviceState *dev, Error **errp)
{
    TdeckCst328State *s = TDECK_CST328(dev);
    qdev_init_gpio_out_named(dev, &s->int_pin, "int", 1);
}

static void tdeck_cst328_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    dc->legacy_reset = tdeck_cst328_reset;
    dc->realize = tdeck_cst328_realize;
    sc->event = tdeck_cst328_event;
    sc->recv = tdeck_cst328_recv;
    sc->send = tdeck_cst328_send;
}

static const TypeInfo tdeck_cst328_info = {
    .name = TYPE_TDECK_CST328,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TdeckCst328State),
    .class_init = tdeck_cst328_class_init,
};

/* ========================================================================= */
/* BHI260AP IMU (stub - ACK only)                                            */
/* ========================================================================= */

#define TYPE_TDECK_BHI260AP "tdeck-bhi260ap"
OBJECT_DECLARE_SIMPLE_TYPE(TdeckBhi260apState, TDECK_BHI260AP)

struct TdeckBhi260apState {
    I2CSlave parent_obj;
};

static int tdeck_bhi260ap_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;
}

static uint8_t tdeck_bhi260ap_recv(I2CSlave *i2c)
{
    return 0;
}

static int tdeck_bhi260ap_send(I2CSlave *i2c, uint8_t data)
{
    return 0;
}

static void tdeck_bhi260ap_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    sc->event = tdeck_bhi260ap_event;
    sc->recv = tdeck_bhi260ap_recv;
    sc->send = tdeck_bhi260ap_send;
}

static const TypeInfo tdeck_bhi260ap_info = {
    .name = TYPE_TDECK_BHI260AP,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TdeckBhi260apState),
    .class_init = tdeck_bhi260ap_class_init,
};

/* ========================================================================= */
/* LTR-553 Light/Proximity Sensor (stub - ACK only)                          */
/* ========================================================================= */

#define TYPE_TDECK_LTR553 "tdeck-ltr553"
OBJECT_DECLARE_SIMPLE_TYPE(TdeckLtr553State, TDECK_LTR553)

struct TdeckLtr553State {
    I2CSlave parent_obj;
};

static int tdeck_ltr553_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;
}

static uint8_t tdeck_ltr553_recv(I2CSlave *i2c)
{
    return 0;
}

static int tdeck_ltr553_send(I2CSlave *i2c, uint8_t data)
{
    return 0;
}

static void tdeck_ltr553_class_init(ObjectClass *klass, void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    sc->event = tdeck_ltr553_event;
    sc->recv = tdeck_ltr553_recv;
    sc->send = tdeck_ltr553_send;
}

static const TypeInfo tdeck_ltr553_info = {
    .name = TYPE_TDECK_LTR553,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TdeckLtr553State),
    .class_init = tdeck_ltr553_class_init,
};

/* ========================================================================= */
/* Registration                                                              */
/* ========================================================================= */

static void tdeck_i2c_devices_register_types(void)
{
    type_register_static(&tdeck_bq25896_info);
    type_register_static(&tdeck_bq27220_info);
    type_register_static(&tdeck_tca8418_info);
    type_register_static(&tdeck_cst328_info);
    type_register_static(&tdeck_bhi260ap_info);
    type_register_static(&tdeck_ltr553_info);
}

type_init(tdeck_i2c_devices_register_types)

/* Public accessors for panel wiring (EPD display model writes these) */
void tdeck_bq27220_set_reg(TdeckBq27220State *s, uint8_t addr, uint8_t val)
{
    if (s && addr < sizeof(s->regs)) {
        s->regs[addr] = val;
    }
}

void tdeck_bq25896_set_reg(TdeckBq25896State *s, uint8_t addr, uint8_t val)
{
    if (s && addr < sizeof(s->regs)) {
        s->regs[addr] = val;
    }
}
