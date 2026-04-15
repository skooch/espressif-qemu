/*
 * ESP32S3 Interrupt Matrix
 *
 * Copyright (c) 2019-2024 Espressif Systems (Shanghai) Co. Ltd.
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
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/xtensa/esp32s3_intc.h"

#define INTMATRIX_RESET_MAP_VALUE 16

#define INTC_DEBUG      0
#define INTC_WARNING    0

#define IRQ_MAP(cpu, input) s->irq_map[cpu][input]

static bool esp32s3_intmatrix_source_is_reserved(int source)
{
    /*
     * These sources are present in the ESP32-S3 source-number space but have
     * null entries in the PAC interrupt vector table used by the current Rust
     * firmware stack. Suppressing them is a QEMU compatibility policy, not an
     * EXTMEM/INTERRUPT_CORE register semantic.
     */
    static const int reserved_sources[] = {15, 23, 33, 34, 46};

    for (int i = 0; i < ARRAY_SIZE(reserved_sources); i++) {
        if (source == reserved_sources[i]) {
            return true;
        }
    }

    return false;
}

static void esp32s3_intmatrix_irq_handler(void *opaque, int n, int level)
{
    Esp32s3IntMatrixState *s = ESP32S3_INTMATRIX(opaque);

    /* Track interrupt source levels for the status registers */
    if (n < 128) {
        if (level) {
            s->irq_levels[n / 32] |= (1u << (n % 32));
        } else {
            s->irq_levels[n / 32] &= ~(1u << (n % 32));
        }
    }

    if (esp32s3_intmatrix_source_is_reserved(n)) {
        return;
    }

    for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
        if (s->outputs[i] == NULL) {
            continue;
        }
        int out_index = IRQ_MAP(i, n);
        for (int int_index = 0; int_index < s->cpu[i]->env.config->nextint; ++int_index) {
            if (s->cpu[i]->env.config->extint[int_index] == out_index) {
                qemu_set_irq(s->outputs[i][int_index], level);
                break;
            }
        }
    }
}

static inline uint8_t* get_map_entry(Esp32s3IntMatrixState* s, hwaddr addr)
{
    int source_index = addr / sizeof(uint32_t);
    if (source_index > ESP32S3_INT_MATRIX_INPUTS * ESP32S3_CPU_COUNT) {
#if INTC_DEBUG
        info_report("%s: source_index %d out of range", __func__, source_index);
#endif // INTC_DEBUG
        return NULL;
    }
    int cpu_index = source_index / ESP32S3_INT_MATRIX_INPUTS;
    source_index = source_index % ESP32S3_INT_MATRIX_INPUTS;
    return &IRQ_MAP(cpu_index, source_index);
}

/*
 * INTERRUPT_CORE0 register layout:
 *   0x000-0x18B: mapping registers (source -> CPU interrupt)
 *   0x18C-0x198: interrupt status registers (4 x 32-bit, read-only)
 *   0x19C:       clock gate register
 *
 * The status registers reflect which peripheral interrupt sources are currently
 * active. We track this via the irq_levels bitmask, which is updated by the
 * IRQ handler when peripherals assert/deassert their interrupt lines.
 */

/* Status register offsets within the INTERRUPT_CORE0 peripheral */
#define INTMATRIX_STATUS_REG0   0x18C
#define INTMATRIX_STATUS_REG3   0x198

/*
 * Mask for interrupt sources that have null entries in the PAC __INTERRUPTS
 * vector table (Vector { _reserved: 0 }). If any of these sources appear
 * as active in the status registers, the firmware dispatcher jumps to PC=0.
 *
 * Null entries at indices 15, 23, 33, 34, 46:
 *   Word 0 (sources 0-31):  bits 15, 23 = 0x00808000
 *   Word 1 (sources 32-63): bits 1, 2, 14 = 0x00004006
 */
#define INTMATRIX_RESERVED_MASK_0  ((1u << 15) | (1u << 23))
#define INTMATRIX_RESERVED_MASK_1  ((1u << (33-32)) | (1u << (34-32)) | (1u << (46-32)))

static uint64_t esp32s3_intmatrix_read(void* opaque, hwaddr addr, unsigned int size)
{
    Esp32s3IntMatrixState *s = ESP32S3_INTMATRIX(opaque);

    /* Interrupt status registers: return actual pending interrupt state.
     *
     * Core 0 status at offsets 0x18C-0x198 (4 words).
     * Core 1 status at offsets 0x98C-0x998 (4 words, 0x800 per CPU).
     * Both return the same irq_levels[] (shared peripheral state). */
    {
        int word = -1;
        if (addr >= INTMATRIX_STATUS_REG0 && addr <= INTMATRIX_STATUS_REG3) {
            word = (addr - INTMATRIX_STATUS_REG0) / 4;
        } else if (addr >= (INTMATRIX_STATUS_REG0 + 0x800) &&
                   addr <= (INTMATRIX_STATUS_REG3 + 0x800)) {
            word = (addr - (INTMATRIX_STATUS_REG0 + 0x800)) / 4;
        }
        if (word >= 0 && word < 4) {
            uint32_t val = s->irq_levels[word];
            if (word == 0) {
                val &= ~INTMATRIX_RESERVED_MASK_0;
            }
            if (word == 1) {
                val &= ~INTMATRIX_RESERVED_MASK_1;
            }
            return val;
        }
    }

    /* Mapping registers: return the CPU interrupt number for this source */
    uint8_t* map_entry = get_map_entry(s, addr);
    return (map_entry != NULL) ? *map_entry : 0;
}

static void esp32s3_intmatrix_write(void* opaque, hwaddr addr, uint64_t value, unsigned int size)
{
#if INTC_DEBUG
    info_report("\x1b[31m[INTC] esp32s3_intmatrix_write  addr = %ld, value=%ld\x1b[0m", addr, value);
#endif // INTC_DEBUG
    Esp32s3IntMatrixState *s = ESP32S3_INTMATRIX(opaque);
    uint8_t* map_entry = get_map_entry(s, addr);
    if (map_entry != NULL) {
        *map_entry = value & 0x1f;
    }
}

static const MemoryRegionOps esp_intmatrix_ops = {
    .read =  esp32s3_intmatrix_read,
    .write = esp32s3_intmatrix_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_intmatrix_reset_hold(Object *obj, ResetType type)
{
    Esp32s3IntMatrixState *s = ESP32S3_INTMATRIX(obj);
    memset(s->irq_map, INTMATRIX_RESET_MAP_VALUE, sizeof(s->irq_map));
    memset(s->irq_levels, 0, sizeof(s->irq_levels));
    for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
        if (s->outputs[i] == NULL) {
            continue;
        }
        for (int int_index = 0; int_index < s->cpu[i]->env.config->nextint; ++int_index) {
            qemu_irq_lower(s->outputs[i][int_index]);
        }
    }
}

static void esp32s3_intmatrix_realize(DeviceState *dev, Error **errp)
{
    Esp32s3IntMatrixState *s = ESP32S3_INTMATRIX(dev);

    for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
        if (s->cpu[i]) {
            s->outputs[i] = xtensa_get_extints(&s->cpu[i]->env);
        }
    }
    esp32s3_intmatrix_reset_hold(OBJECT(dev), RESET_TYPE_COLD);
}

static void esp32s3_intmatrix_init(Object *obj)
{
    Esp32s3IntMatrixState *s = ESP32S3_INTMATRIX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp_intmatrix_ops, s,
                          TYPE_ESP32S3_INTMATRIX, ESP32S3_INT_MATRIX_INPUTS * ESP32S3_CPU_COUNT * sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in(DEVICE(s), esp32s3_intmatrix_irq_handler, ESP32S3_INT_MATRIX_INPUTS);
}

static Property esp32s3_intmatrix_properties[] = {
    DEFINE_PROP_LINK("cpu0", Esp32s3IntMatrixState, cpu[0], TYPE_XTENSA_CPU, XtensaCPU *),
    DEFINE_PROP_LINK("cpu1", Esp32s3IntMatrixState, cpu[1], TYPE_XTENSA_CPU, XtensaCPU *),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_intmatrix_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_intmatrix_reset_hold;
    dc->realize = esp32s3_intmatrix_realize;
    device_class_set_props(dc, esp32s3_intmatrix_properties);
}

static const TypeInfo esp32s3_intmatrix_info = {
    .name = TYPE_ESP32S3_INTMATRIX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3IntMatrixState),
    .instance_init = esp32s3_intmatrix_init,
    .class_init = esp32s3_intmatrix_class_init
};

static void esp32s3_intmatrix_register_types(void)
{
    type_register_static(&esp32s3_intmatrix_info);
}

type_init(esp32s3_intmatrix_register_types)
