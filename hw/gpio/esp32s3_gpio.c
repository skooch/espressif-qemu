/*
 * ESP32-S3 GPIO emulation
 *
 * Implements the GPIO matrix register interface:
 *   - Output registers (OUT, OUT_W1TS, OUT_W1TC) for GPIO0-48
 *   - Input registers (IN, IN1) reflecting pin levels
 *   - Output enable registers
 *   - Per-pin config (PINn) with interrupt type
 *   - Function select (IN_SEL_CFG, OUT_SEL_CFG) for signal routing
 *   - Interrupt status with edge/level detection
 *
 * External device models call esp32s3_gpio_set_input() to drive pin levels.
 * The model generates interrupts based on per-pin INT_TYPE config.
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32s3_gpio.h"


static void esp32s3_gpio_update_irq(ESP32S3GPIOState *s)
{
    bool irq = (s->status[0] != 0) || (s->status[1] != 0);
    qemu_set_irq(s->irq_cpu0, irq ? 1 : 0);
}

/*
 * Check interrupt condition for a single pin based on its INT_TYPE config
 * and the current/previous level.
 */
static void esp32s3_gpio_check_int(ESP32S3GPIOState *s, int gpio_num,
                                    bool old_level, bool new_level)
{
    uint32_t pin_cfg = s->pin_reg[gpio_num];
    int int_type = (pin_cfg & GPIO_PIN_INT_TYPE_MASK) >> GPIO_PIN_INT_TYPE_SHIFT;

    bool trigger = false;
    switch (int_type) {
    case GPIO_INT_DISABLE:
        break;
    case GPIO_INT_RISING:
        trigger = !old_level && new_level;
        break;
    case GPIO_INT_FALLING:
        trigger = old_level && !new_level;
        break;
    case GPIO_INT_BOTH:
        trigger = old_level != new_level;
        break;
    case GPIO_INT_LOW:
        trigger = !new_level;
        break;
    case GPIO_INT_HIGH:
        trigger = new_level;
        break;
    }

    if (trigger) {
        int idx = gpio_num / 32;
        int bit = gpio_num % 32;
        s->status[idx] |= (1u << bit);
        esp32s3_gpio_update_irq(s);
    }
}

/*
 * Public API: set a GPIO input level from an external device model.
 * This updates the IN register and checks for interrupt conditions.
 */
void esp32s3_gpio_set_input(ESP32S3GPIOState *s, int gpio_num, bool level)
{
    if (gpio_num < 0 || gpio_num >= ESP32S3_GPIO_COUNT) {
        return;
    }

    int idx = gpio_num / 32;
    int bit = gpio_num % 32;

    bool old_level = (s->in_levels[idx] >> bit) & 1;

    if (level) {
        s->in_levels[idx] |= (1u << bit);
    } else {
        s->in_levels[idx] &= ~(1u << bit);
    }

    esp32s3_gpio_check_int(s, gpio_num, old_level, level);
}

static uint64_t esp32s3_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);
    uint64_t r = 0;

    switch (addr) {
    /* Output data */
    case GPIO_OUT_REG:
        r = s->out[0];
        break;
    case GPIO_OUT1_REG:
        r = s->out[1];
        break;

    /* Output enable */
    case GPIO_ENABLE_REG:
        r = s->enable[0];
        break;
    case GPIO_ENABLE1_REG:
        r = s->enable[1];
        break;

    /* Input (reflects pin levels from external devices + loopback from output) */
    case GPIO_IN_REG:
        /* Combine external input levels with output-enabled pins */
        r = s->in_levels[0] | (s->out[0] & s->enable[0]);
        break;
    case GPIO_IN1_REG:
        r = s->in_levels[1] | (s->out[1] & s->enable[1]);
        break;

    /* Strap mode */
    case GPIO_STRAP_REG:
        r = s->parent.strap_mode;
        break;

    /* Interrupt status */
    case GPIO_STATUS_REG:
        r = s->status[0];
        break;
    case GPIO_STATUS1_REG:
        r = s->status[1];
        break;

    default:
        /* Per-pin config registers */
        if (addr >= GPIO_PIN0_REG &&
            addr < GPIO_PIN0_REG + ESP32S3_GPIO_COUNT * 4 &&
            (addr - GPIO_PIN0_REG) % 4 == 0) {
            int pin = (addr - GPIO_PIN0_REG) / 4;
            r = s->pin_reg[pin];
        }
        /* Input function select */
        else if (addr >= GPIO_FUNC_IN_SEL_CFG_REG(0) &&
                 addr < GPIO_FUNC_IN_SEL_CFG_REG(256) &&
                 (addr - GPIO_FUNC_IN_SEL_CFG_REG(0)) % 4 == 0) {
            int func = (addr - GPIO_FUNC_IN_SEL_CFG_REG(0)) / 4;
            r = s->func_in_sel[func];
        }
        /* Output function select */
        else if (addr >= GPIO_FUNC_OUT_SEL_CFG_REG(0) &&
                 addr < GPIO_FUNC_OUT_SEL_CFG_REG(ESP32S3_GPIO_COUNT) &&
                 (addr - GPIO_FUNC_OUT_SEL_CFG_REG(0)) % 4 == 0) {
            int pin = (addr - GPIO_FUNC_OUT_SEL_CFG_REG(0)) / 4;
            r = s->func_out_sel[pin];
        }
        break;
    }

    return r;
}

static void esp32s3_gpio_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned int size)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(opaque);

    switch (addr) {
    /* Output data: direct write, set, clear */
    case GPIO_OUT_REG:
        s->out[0] = (uint32_t)value;
        break;
    case GPIO_OUT_W1TS_REG:
        s->out[0] |= (uint32_t)value;
        break;
    case GPIO_OUT_W1TC_REG:
        s->out[0] &= ~(uint32_t)value;
        break;
    case GPIO_OUT1_REG:
        s->out[1] = (uint32_t)value;
        break;
    case GPIO_OUT1_W1TS_REG:
        s->out[1] |= (uint32_t)value;
        break;
    case GPIO_OUT1_W1TC_REG:
        s->out[1] &= ~(uint32_t)value;
        break;

    /* Output enable: direct write, set, clear */
    case GPIO_ENABLE_REG:
        s->enable[0] = (uint32_t)value;
        break;
    case GPIO_ENABLE_W1TS_REG:
        s->enable[0] |= (uint32_t)value;
        break;
    case GPIO_ENABLE_W1TC_REG:
        s->enable[0] &= ~(uint32_t)value;
        break;
    case GPIO_ENABLE1_REG:
        s->enable[1] = (uint32_t)value;
        break;
    case GPIO_ENABLE1_W1TS_REG:
        s->enable[1] |= (uint32_t)value;
        break;
    case GPIO_ENABLE1_W1TC_REG:
        s->enable[1] &= ~(uint32_t)value;
        break;

    /* Interrupt status: write-1-to-clear */
    case GPIO_STATUS_W1TC_REG:
        s->status[0] &= ~(uint32_t)value;
        esp32s3_gpio_update_irq(s);
        break;
    case GPIO_STATUS_W1TS_REG:
        s->status[0] |= (uint32_t)value;
        esp32s3_gpio_update_irq(s);
        break;
    case GPIO_STATUS1_W1TC_REG:
        s->status[1] &= ~(uint32_t)value;
        esp32s3_gpio_update_irq(s);
        break;
    case GPIO_STATUS1_W1TS_REG:
        s->status[1] |= (uint32_t)value;
        esp32s3_gpio_update_irq(s);
        break;

    default:
        /* Per-pin config registers */
        if (addr >= GPIO_PIN0_REG &&
            addr < GPIO_PIN0_REG + ESP32S3_GPIO_COUNT * 4 &&
            (addr - GPIO_PIN0_REG) % 4 == 0) {
            int pin = (addr - GPIO_PIN0_REG) / 4;
            s->pin_reg[pin] = (uint32_t)value;

            /* Re-evaluate level interrupts: if the current pin level
             * already satisfies the newly configured interrupt type,
             * fire the interrupt immediately. This is needed for
             * wait_for_high/wait_for_low on pins that are already
             * at the target level. */
            int idx = pin / 32;
            int bit = pin % 32;
            bool level = (s->in_levels[idx] >> bit) & 1;
            esp32s3_gpio_check_int(s, pin, level, level);
        }
        /* Input function select */
        else if (addr >= GPIO_FUNC_IN_SEL_CFG_REG(0) &&
                 addr < GPIO_FUNC_IN_SEL_CFG_REG(256) &&
                 (addr - GPIO_FUNC_IN_SEL_CFG_REG(0)) % 4 == 0) {
            int func = (addr - GPIO_FUNC_IN_SEL_CFG_REG(0)) / 4;
            s->func_in_sel[func] = (uint32_t)value;
        }
        /* Output function select */
        else if (addr >= GPIO_FUNC_OUT_SEL_CFG_REG(0) &&
                 addr < GPIO_FUNC_OUT_SEL_CFG_REG(ESP32S3_GPIO_COUNT) &&
                 (addr - GPIO_FUNC_OUT_SEL_CFG_REG(0)) % 4 == 0) {
            int pin = (addr - GPIO_FUNC_OUT_SEL_CFG_REG(0)) / 4;
            s->func_out_sel[pin] = (uint32_t)value;
        }
        break;
    }
}

static const MemoryRegionOps esp32s3_gpio_ops = {
    .read =  esp32s3_gpio_read,
    .write = esp32s3_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_gpio_reset_hold(Object *obj, ResetType type)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(obj);

    memset(s->out, 0, sizeof(s->out));
    memset(s->enable, 0, sizeof(s->enable));
    memset(s->in_levels, 0, sizeof(s->in_levels));
    memset(s->status, 0, sizeof(s->status));
    memset(s->pin_reg, 0, sizeof(s->pin_reg));
    memset(s->func_in_sel, 0, sizeof(s->func_in_sel));
    memset(s->func_out_sel, 0, sizeof(s->func_out_sel));

    /* Default: all output function selects to 0x100 (GPIO matrix bypass) */
    for (int i = 0; i < ESP32S3_GPIO_COUNT; i++) {
        s->func_out_sel[i] = 0x100;
    }
}

static void esp32s3_gpio_realize(DeviceState *dev, Error **errp)
{
}

static void esp32s3_gpio_init(Object *obj)
{
    ESP32S3GPIOState *s = ESP32S3_GPIO(obj);

    /* Set the default value for the property */
    object_property_set_int(obj, "strap_mode", ESP32S3_STRAP_MODE_FLASH_BOOT,
                            &error_fatal);

    /* Re-initialize the parent memory region with our ops and larger size.
     * The parent esp32_gpio_init already called memory_region_init_io + sysbus_init_mmio,
     * but we need different ops and a larger region (0x700 vs 0x1000). Since the mmio
     * slot is already registered, just reinit the MemoryRegion in place. */
    memory_region_init_io(&s->parent.iomem, obj, &esp32s3_gpio_ops, s,
                          TYPE_ESP32S3_GPIO, ESP32S3_GPIO_IO_SIZE);

    /* IRQ output to interrupt matrix */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_cpu0);
}

static void esp32s3_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_gpio_reset_hold;
    dc->realize = esp32s3_gpio_realize;
}

static const TypeInfo esp32s3_gpio_info = {
    .name = TYPE_ESP32S3_GPIO,
    .parent = TYPE_ESP32_GPIO,
    .instance_size = sizeof(ESP32S3GPIOState),
    .instance_init = esp32s3_gpio_init,
    .class_init = esp32s3_gpio_class_init,
    .class_size = sizeof(ESP32S3GPIOClass),
};

static void esp32s3_gpio_register_types(void)
{
    type_register_static(&esp32s3_gpio_info);
}

type_init(esp32s3_gpio_register_types)
