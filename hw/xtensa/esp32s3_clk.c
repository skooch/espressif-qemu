/*
 * ESP32-S3 Clocks definition
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/clock.h"
#include "hw/xtensa/esp32s3_clk.h"
#include "hw/xtensa/esp32s3_clk_defs.h"
#include "target/xtensa/cpu.h"

#define CLOCK_DEBUG      0
#define CLOCK_WARNING    0
#define SYSTEM_CPU_PER_CONF_SUPPORTED_MASK \
    (R_SYSTEM_CPU_PER_CONF_CPU_WAITI_DELAY_NUM_MASK | \
     R_SYSTEM_CPU_PER_CONF_CPU_WAIT_MODE_FORCE_ON_MASK | \
     R_SYSTEM_CPU_PER_CONF_PLL_FREQ_SEL_MASK | \
     R_SYSTEM_CPU_PER_CONF_CPUPERIOD_SEL_MASK)
#define SYSTEM_SYSCLK_CONF_SUPPORTED_MASK \
    (R_SYSTEM_SYSCLK_CONF_CLK_DIV_EN_MASK | \
     R_SYSTEM_SYSCLK_CONF_CLK_XTAL_FREQ_MASK | \
     R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_MASK | \
     R_SYSTEM_SYSCLK_CONF_PRE_DIV_CNT_MASK)

static uint32_t esp32s3_read_cpu_intr(ESP32S3ClockState *s, uint32_t index)
{
    return (s->levels >> index) & 1;
}


static void esp32s3_write_cpu_intr(ESP32S3ClockState *s, uint32_t index, uint32_t value)
{
    const uint32_t field = FIELD_EX32(value, SYSTEM_CPU_INTR_FROM_CPU_0, CPU_INTR_FROM_CPU_0);
    if (field) {
        s->levels |= BIT(index);
        qemu_set_irq(s->irqs[index], 1);
    } else {
        s->levels &= ~BIT(index);
        qemu_set_irq(s->irqs[index], 0);
    }
}

static uint32_t esp32s3_clock_get_ext_dev_enc_dec_ctrl(ESP32S3ClockState *s)
{
    return s->sys_ext_dev_enc_dec_ctrl;
}

void esp32s3_clock_propagate_rates(ESP32S3ClockState *s)
{
    uint32_t cpu_hz = esp32s3_clock_get_cpu_freq(s);

    for (size_t i = 0; i < ARRAY_SIZE(s->cpu); i++) {
        XtensaCPU *cpu;

        if (!s->cpu[i]) {
            continue;
        }

        cpu = XTENSA_CPU(s->cpu[i]);
        if (cpu->clock) {
            clock_update_hz(cpu->clock, cpu_hz);
        }
    }
}

void esp32s3_clock_apply_rtc_soc_clk(ESP32S3ClockState *s,
                                     uint32_t soc_clk_sel,
                                     uint32_t xtal_freq_hz)
{
    uint32_t xtal_mhz = xtal_freq_hz ? (xtal_freq_hz / 1000000u) : 40;

    if (xtal_mhz == 0) {
        xtal_mhz = 40;
    }

    s->sysclk = FIELD_DP32(s->sysclk, SYSTEM_SYSCLK_CONF, SOC_CLK_SEL,
                           soc_clk_sel);
    s->sysclk = FIELD_DP32(s->sysclk, SYSTEM_SYSCLK_CONF, CLK_XTAL_FREQ,
                           xtal_mhz);
    esp32s3_clock_propagate_rates(s);
}

uint32_t esp32s3_clock_get_xtal_freq(ESP32S3ClockState *s)
{
    uint32_t mhz = FIELD_EX32(s->sysclk, SYSTEM_SYSCLK_CONF, CLK_XTAL_FREQ);

    if (mhz == 0) {
        mhz = 40;
    }

    return mhz * 1000000u;
}

uint32_t esp32s3_clock_get_cpu_freq(ESP32S3ClockState *s)
{
    switch (FIELD_EX32(s->sysclk, SYSTEM_SYSCLK_CONF, SOC_CLK_SEL)) {
    case ESP32S3_CLK_SEL_XTAL:
        return esp32s3_clock_get_xtal_freq(s);
    case ESP32S3_CLK_SEL_RCFAST:
        return 17500000u;
    case ESP32S3_CLK_SEL_PLL:
    default:
        switch (FIELD_EX32(s->cpuperconf, SYSTEM_CPU_PER_CONF, CPUPERIOD_SEL)) {
        case ESP32S3_PERIOD_SEL_160:
            return 160000000u;
        case ESP32S3_PERIOD_SEL_80:
        default:
            return 80000000u;
        }
    }
}

static void esp32s3_clock_update_cpu_per_conf(ESP32S3ClockState *s,
                                              uint32_t value)
{
    s->cpuperconf = value & SYSTEM_CPU_PER_CONF_SUPPORTED_MASK;
    esp32s3_clock_propagate_rates(s);
}

static void esp32s3_clock_update_sysclk_conf(ESP32S3ClockState *s,
                                             uint32_t value)
{
    s->sysclk = value & SYSTEM_SYSCLK_CONF_SUPPORTED_MASK;
    esp32s3_clock_propagate_rates(s);
}

uint32_t esp32s3_clock_get_apb_freq(ESP32S3ClockState *s)
{
    uint32_t cpu_hz = esp32s3_clock_get_cpu_freq(s);

    return MIN(cpu_hz, 80000000u);
}

static uint64_t esp32s3_clock_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(opaque);
    uint64_t r = 0;

    switch(addr) {
        case A_SYSTEM_CORE_1_CONTROL_0_REG:
            r = s->core1_control0;
            break;
        case A_SYSTEM_CORE_1_CONTROL_1_REG:
                r = s->app_cpu_addr;
            break;
        case A_SYSTEM_CPU_PER_CONF:
            r = s->cpuperconf;
            break;
        case A_SYSTEM_PERIP_CLK_EN0:
            r = s->perip_clk_en0;
            break;
        case A_SYSTEM_PERIP_CLK_EN1:
            r = s->perip_clk_en1;
            break;
        case A_SYSTEM_PERIP_RST_EN0:
            r = s->perip_rst_en0;
            break;
        case A_SYSTEM_PERIP_RST_EN1:
            r = s->perip_rst_en1;
            break;
        case A_SYSTEM_CLOCK_GATE:
            r = s->clock_gate;
            break;
        case A_SYSTEM_SYSCLK_CONF:
            r = s->sysclk;
            break;
        case A_SYSTEM_CPU_INTR_FROM_CPU_0:
        case A_SYSTEM_CPU_INTR_FROM_CPU_1:
        case A_SYSTEM_CPU_INTR_FROM_CPU_2:
        case A_SYSTEM_CPU_INTR_FROM_CPU_3:
            r = esp32s3_read_cpu_intr(s, (addr - A_SYSTEM_CPU_INTR_FROM_CPU_0) / sizeof(uint32_t));
            break;
        case A_SYSTEM_EXTERNAL_DEVICE_ENCRYPT_DECRYPT_CONTROL:
            r = s->sys_ext_dev_enc_dec_ctrl;
            break;
        default:
#if CLOCK_WARNING
            warn_report("[CLOCK] Unsupported read from %08lx\n", addr);
#endif
            break;
    }
    return r;
}

static void esp32s3_clock_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(opaque);

    switch(addr) {
        case A_SYSTEM_CORE_1_CONTROL_0_REG: {
            uint32_t old = s->core1_control0;
            s->core1_control0 = (uint32_t)value;
            /* Bit 0 = RUNSTALL for Core 1 */
            bool stall_new = value & 1;
            bool stall_old = old & 1;
            if (stall_new != stall_old && s->core1_runstall) {
                qemu_set_irq(s->core1_runstall, stall_new);
            }
            break;
        }
        case A_SYSTEM_CORE_1_CONTROL_1_REG:
                s->app_cpu_addr = (uint32_t)value;
            break;
        case A_SYSTEM_CPU_PER_CONF:
            esp32s3_clock_update_cpu_per_conf(s, (uint32_t)value);
            break;
        case A_SYSTEM_PERIP_CLK_EN0:
            s->perip_clk_en0 = (uint32_t)value;
            break;
        case A_SYSTEM_PERIP_CLK_EN1:
            s->perip_clk_en1 = (uint32_t)value;
            break;
        case A_SYSTEM_PERIP_RST_EN0:
            s->perip_rst_en0 = (uint32_t)value;
            break;
        case A_SYSTEM_PERIP_RST_EN1:
            s->perip_rst_en1 = (uint32_t)value;
            break;
        case A_SYSTEM_CLOCK_GATE:
            s->clock_gate = value & R_SYSTEM_CLOCK_GATE_CLK_EN_MASK;
            break;
        case A_SYSTEM_SYSCLK_CONF:
            esp32s3_clock_update_sysclk_conf(s, (uint32_t)value);
            break;
        case A_SYSTEM_CPU_INTR_FROM_CPU_0:
        case A_SYSTEM_CPU_INTR_FROM_CPU_1:
        case A_SYSTEM_CPU_INTR_FROM_CPU_2:
        case A_SYSTEM_CPU_INTR_FROM_CPU_3:
            esp32s3_write_cpu_intr(s, (addr - A_SYSTEM_CPU_INTR_FROM_CPU_0) / sizeof(uint32_t), value);
            break;
        case A_SYSTEM_EXTERNAL_DEVICE_ENCRYPT_DECRYPT_CONTROL:
            s->sys_ext_dev_enc_dec_ctrl = value;
            break;
        default:
#if CLOCK_WARNING
            warn_report("[CLOCK] Unsupported write to %08lx (%08lx)\n", addr, value);
#endif
            break;
    }
}

static const MemoryRegionOps esp32s3_clock_ops = {
    .read =  esp32s3_clock_read,
    .write = esp32s3_clock_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_clock_reset_hold(Object *obj, ResetType type)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(obj);
    /* On board reset, set the proper clocks and dividers */
    s->sysclk = ( 1 << R_SYSTEM_SYSCLK_CONF_PRE_DIV_CNT_SHIFT) |
                (ESP32S3_CLK_SEL_PLL << R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_SHIFT) |
                (40 << R_SYSTEM_SYSCLK_CONF_CLK_XTAL_FREQ_SHIFT) |
                ( 1 << R_SYSTEM_SYSCLK_CONF_CLK_DIV_EN_SHIFT);

    /* Divider for PLL clock and APB  frequency */
    s->cpuperconf = (ESP32S3_PERIOD_SEL_80 << R_SYSTEM_CPU_PER_CONF_CPUPERIOD_SEL_SHIFT) |
                    (ESP32S3_FREQ_SEL_PLL_480 << R_SYSTEM_CPU_PER_CONF_PLL_FREQ_SEL_SHIFT) |
                    R_SYSTEM_CPU_PER_CONF_CPU_WAIT_MODE_FORCE_ON_MASK;
    s->perip_clk_en0 = 0;
    s->perip_clk_en1 = 0;
    s->perip_rst_en0 = 0;
    s->perip_rst_en1 = 0;
    s->clock_gate = R_SYSTEM_CLOCK_GATE_CLK_EN_MASK;
    s->core1_control0 = 0;

    esp32s3_clock_propagate_rates(s);

    /* Initialize the IRQs */
    s->levels = 0;
    for (int i = 0 ; i < ESP32S3_SYSTEM_CPU_INTR_COUNT; i++) {
        qemu_irq_lower(s->irqs[i]);
    }
    qemu_irq_lower(s->core1_runstall);
}

static void esp32s3_clock_realize(DeviceState *dev, Error **errp)
{
    /* Initialize the registers */
    esp32s3_clock_reset_hold(OBJECT(dev), RESET_TYPE_COLD);
}

static void esp32s3_clock_init(Object *obj)
{
    ESP32S3ClockState *s = ESP32S3_CLOCK(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_clock_ops, s,
                          TYPE_ESP32S3_CLOCK, A_SYSTEM_COMB_PVT_ERR_HVT_SITE3 + sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->iomem);

    /* Initialize the output IRQ lines used to manually trigger interrupts */
    for (uint64_t i = 0; i < ESP32S3_SYSTEM_CPU_INTR_COUNT; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }
    qdev_init_gpio_out_named(DEVICE(sbd), &s->core1_runstall,
                             ESP32S3_CLOCK_CORE1_RUNSTALL_GPIO, 1);
}

static void esp32s3_clock_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ESP32S3ClockClass* esp32s3_clock = ESP32S3_CLOCK_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_clock_reset_hold;
    dc->realize = esp32s3_clock_realize;

    esp32s3_clock->get_ext_dev_enc_dec_ctrl = esp32s3_clock_get_ext_dev_enc_dec_ctrl;
}

static const TypeInfo esp32s3_cache_info = {
    .name = TYPE_ESP32S3_CLOCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3ClockState),
    .instance_init = esp32s3_clock_init,
    .class_init = esp32s3_clock_class_init,
    .class_size = sizeof(ESP32S3ClockClass)
};

static void esp32s3_cache_register_types(void)
{
    type_register_static(&esp32s3_cache_info);
}

type_init(esp32s3_cache_register_types)
