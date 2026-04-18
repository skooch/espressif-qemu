/*
 * ESP32 RTC_CNTL (RTC block controller) device
 *
 * Copyright (c) 2019-2024 Espressif Systems (Shanghai) Co. Ltd.
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
#include "hw/misc/esp32s3_reg.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "hw/gpio/esp32s3_gpio.h"

static void esp32s3_rtc_update_cpu_stall(Esp32s3RtcCntlState* s);
static void esp32s3_rtc_update_clk(Esp32s3RtcCntlState* s);

/* wakeup_ena bits (at [31:15] of WAKEUP_STATE, so trigger bit N = reg bit N+15) */
#define WAKEUP_ENA_EXT1_BIT    (1 << 16)  /* ExtEvent1Trig = trigger bit 1 */

static void esp32s3_rtc_set_sleep_state(Esp32s3RtcCntlState *s,
                                        Esp32s3RtcSleepState state)
{
    bool old_sleeping = s->sleep_state == ESP32S3_RTC_SLEEP_SLEEPING;
    bool new_sleeping = state == ESP32S3_RTC_SLEEP_SLEEPING;

    s->sleep_state = state;

    if (old_sleeping != new_sleeping && s->light_sleep_req) {
        qemu_set_irq(s->light_sleep_req, new_sleeping);
    }
}

static void esp32s3_rtc_slp_timer_cb(void *opaque)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(opaque);
    if (s->sleep_state != ESP32S3_RTC_SLEEP_SLEEPING) {
        return;
    }
    esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_WOKE);
    s->slp_wakeup_cause = R_RTC_CNTL_SLP_WAKEUP_CAUSE_TIMER_MASK;
    s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK;
}

void esp32s3_rtc_gpio_wakeup_notify(Esp32s3RtcCntlState *s, int gpio_num)
{
    if (s->sleep_state != ESP32S3_RTC_SLEEP_SLEEPING) {
        return;
    }
    bool gpio_en = FIELD_EX32(s->wakeup_state, RTC_CNTL_WAKEUP_STATE,
                              GPIO_WAKEUP_EN);
    bool ext1_en = s->wakeup_state & WAKEUP_ENA_EXT1_BIT;

    /* Check digital GPIO wakeup */
    if (gpio_en && s->gpio &&
        gpio_num >= 0 && gpio_num < ESP32S3_GPIO_COUNT) {
        uint32_t pin_cfg = s->gpio->pin_reg[gpio_num];
        bool wakeup_enable = (pin_cfg >> 10) & 1;
        int int_type = (pin_cfg >> GPIO_PIN_INT_TYPE_SHIFT) & 0x7;

        if (wakeup_enable && int_type == GPIO_INT_LOW) {
            esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_WOKE);
            s->slp_wakeup_cause = R_RTC_CNTL_SLP_WAKEUP_CAUSE_GPIO_MASK;
            s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK;
            timer_del(&s->slp_timer);
            return;
        }
    }

    /* Check EXT1 wakeup: is the notified pin in the EXT1 selection bitmap? */
    if (ext1_en && s->gpio) {
        uint32_t ext1_sel = FIELD_EX32(s->ext_wakeup1, RTC_CNTL_EXT_WAKEUP1,
                                       EXT_WAKEUP1_SEL);
        bool wake_on_high = FIELD_EX32(s->ext_wakeup_conf,
                                       RTC_CNTL_EXT_WAKEUP_CONF,
                                       EXT_WAKEUP1_LV);
        bool wake_on_low = !wake_on_high;
        int rtc_pin = gpio_num;  /* RTC GPIO N = GPIO N on ESP32-S3 */
        if (rtc_pin < 22 && (ext1_sel & (1 << rtc_pin))) {
            int bank = gpio_num / 32;
            int bit = gpio_num % 32;
            bool pin_level = (s->gpio->in_levels[bank] >> bit) & 1;

            if (wake_on_low ? !pin_level : pin_level) {
                esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_WOKE);
                s->slp_wakeup_cause = R_RTC_CNTL_SLP_WAKEUP_CAUSE_EXT1_MASK;
                s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK;
                timer_del(&s->slp_timer);
                return;
            }
        }
    }
}

static void esp32s3_rtc_enter_sleep(Esp32s3RtcCntlState *s)
{
    bool timer_en = FIELD_EX32(s->wakeup_state, RTC_CNTL_WAKEUP_STATE,
                               TIMER_WAKEUP_EN);
    bool gpio_en = FIELD_EX32(s->wakeup_state, RTC_CNTL_WAKEUP_STATE,
                              GPIO_WAKEUP_EN);
    bool ext1_en = s->wakeup_state & WAKEUP_ENA_EXT1_BIT;

    esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_REQUESTED);

    /* Check for immediate reject: GPIO wakeup pin already at trigger level */
    if (gpio_en && s->gpio) {
        bool reject_en = FIELD_EX32(s->slp_reject_conf,
                                     RTC_CNTL_SLP_REJECT_CONF,
                                     LIGHT_SLP_REJECT_EN);
        if (reject_en) {
            int bank = 15 / 32;
            int bit = 15 % 32;
            uint32_t pin_cfg = s->gpio->pin_reg[15];
            int int_type = (pin_cfg >> GPIO_PIN_INT_TYPE_SHIFT) & 0x7;
            bool wakeup_enable = (pin_cfg >> 10) & 1;
            if (wakeup_enable && int_type == GPIO_INT_LOW) {
                bool pin_level = (s->gpio->in_levels[bank] >> bit) & 1;
                if (!pin_level) {
                    esp32s3_rtc_set_sleep_state(s,
                                                ESP32S3_RTC_SLEEP_REJECTED);
                    s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_REJECT_MASK;
                    return;
                }
            }
        }
    }

    /* Check EXT1: if selected pin is already at trigger level, wake immediately.
     * The firmware configures GPIO15 (RTC_GPIO15) for EXT1 LOW-level wake.
     * EXT1 operates in the RTC always-on domain, independent of digital GPIO. */
    if (ext1_en && s->gpio) {
        uint32_t ext1_sel = FIELD_EX32(s->ext_wakeup1, RTC_CNTL_EXT_WAKEUP1,
                                       EXT_WAKEUP1_SEL);
        bool wake_on_high = FIELD_EX32(s->ext_wakeup_conf,
                                       RTC_CNTL_EXT_WAKEUP_CONF,
                                       EXT_WAKEUP1_LV);
        bool wake_on_low = !wake_on_high;

        /* Check each selected RTC GPIO pin.
         * RTC_GPIO15 = GPIO15 on ESP32-S3 (direct mapping for GPIOs 0-21). */
        for (int rtc_pin = 0; rtc_pin < 22; rtc_pin++) {
            if (!(ext1_sel & (1 << rtc_pin))) {
                continue;
            }
            int gpio_num = rtc_pin;  /* RTC GPIO N = GPIO N on ESP32-S3 */
            int bank = gpio_num / 32;
            int bit = gpio_num % 32;
            bool pin_level = (s->gpio->in_levels[bank] >> bit) & 1;
            if (wake_on_low ? !pin_level : pin_level) {
                /* Pin is already at wake level — immediate wake, not reject.
                 * This is the normal case: TCA8418 holds INT low when events
                 * are pending, and the firmware disables GPIO interrupt before
                 * configuring EXT1 so no race. */
                esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_WOKE);
                s->slp_wakeup_cause = R_RTC_CNTL_SLP_WAKEUP_CAUSE_EXT1_MASK;
                s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK;
                return;
            }
        }
    }

    esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_SLEEPING);

    if (timer_en) {
        uint32_t alarm_lo = s->slp_timer0;
        uint32_t alarm_hi = s->slp_timer1;
        bool alarm_en = FIELD_EX32(alarm_hi, RTC_CNTL_SLP_TIMER1, MAIN_TIMER_ALARM_EN);
        if (alarm_en) {
            uint64_t alarm_ticks = ((uint64_t)(alarm_hi & 0xFFFF) << 32) | alarm_lo;
            int64_t alarm_ns = muldiv64(alarm_ticks, NANOSECONDS_PER_SECOND,
                                         s->rtc_slowclk_freq);
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod(&s->slp_timer, now + alarm_ns);
        }
    }
}

static uint32_t esp32s3_rtc_state0_read(Esp32s3RtcCntlState *s)
{
    uint32_t r = 0;

    switch (s->sleep_state) {
    case ESP32S3_RTC_SLEEP_REQUESTED:
    case ESP32S3_RTC_SLEEP_SLEEPING:
        r = FIELD_DP32(r, RTC_CNTL_STATE0, SLEEP_EN, 1);
        break;
    case ESP32S3_RTC_SLEEP_REJECTED:
        r = FIELD_DP32(r, RTC_CNTL_STATE0, SLP_REJECT, 1);
        break;
    case ESP32S3_RTC_SLEEP_WOKE:
        r = FIELD_DP32(r, RTC_CNTL_STATE0, SLP_WAKEUP, 1);
        break;
    case ESP32S3_RTC_SLEEP_AWAKE:
    default:
        break;
    }

    return r;
}

static uint64_t esp32s3_rtc_cntl_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(opaque);
    uint64_t r = 0;
    switch (addr) {
    case A_RTC_CNTL_OPTIONS0:
        r = s->options0_reg;
        break;
    case A_RTC_CNTL_TIME_UPDATE:
        r = R_RTC_CNTL_TIME_UPDATE_VALID_MASK;
        break;
    case A_RTC_CNTL_TIME0:
        r = s->time_reg & UINT32_MAX;
        break;
    case A_RTC_CNTL_TIME1:
        r = s->time_reg >> 32;
        break;

    case A_RTC_CNTL_SLP_TIMER0:
        r = s->slp_timer0;
        break;

    case A_RTC_CNTL_SLP_TIMER1:
        r = s->slp_timer1 & 0xffff;
        break;

    case A_RTC_CNTL_STATE0:
        r = esp32s3_rtc_state0_read(s);
        break;

    case A_RTC_CNTL_RESET_STATE:
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, RESET_CAUSE_PROCPU, s->reset_cause[0]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, RESET_CAUSE_APPCPU, s->reset_cause[1]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, PROCPU_STAT_VECTOR_SEL, s->stat_vector_sel[0]);
        r = FIELD_DP32(r, RTC_CNTL_RESET_STATE, APPCPU_STAT_VECTOR_SEL, s->stat_vector_sel[1]);
        break;

    case A_RTC_CNTL_STORE0:
    case A_RTC_CNTL_STORE1:
    case A_RTC_CNTL_STORE2:
    case A_RTC_CNTL_STORE3:
        r = s->scratch_reg[(addr - A_RTC_CNTL_STORE0) / 4];
        break;

    case A_RTC_CNTL_WAKEUP_STATE:
        r = s->wakeup_state;
        break;

    case A_RTC_CNTL_EXT_WAKEUP_CONF:
        r = s->ext_wakeup_conf;
        break;

    case A_RTC_CNTL_SLP_REJECT_CONF:
        r = s->slp_reject_conf;
        break;

    case A_RTC_CNTL_CLK_CONF:
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, SOC_CLK_SEL, s->soc_clk);
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL, s->rtc_fastclk);
        r = FIELD_DP32(r, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL, s->rtc_slowclk);
        break;

    case A_RTC_CNTL_SW_CPU_STALL:
        r = s->sw_cpu_stall_reg;
        break;

    case A_RTC_CNTL_WDTWPROTECT:
        r = s->wdt_wprotect;
        break;

    case A_RTC_CNTL_SWD_CONF:
        r = s->swd_conf;
        break;

    case A_RTC_CNTL_SWD_WPROTECT:
        r = s->swd_wprotect;
        break;

    case A_RTC_CNTL_STORE4:
    case A_RTC_CNTL_STORE5:
    case A_RTC_CNTL_STORE6:
    case A_RTC_CNTL_STORE7:
        r = s->scratch_reg[(addr - A_RTC_CNTL_STORE4) / 4 + 4];
        break;

    case A_RTC_CNTL_INT_RAW:
        r = s->int_raw;
        break;

    case A_RTC_CNTL_SLP_WAKEUP_CAUSE:
        r = s->slp_wakeup_cause;
        break;

    case A_RTC_CNTL_PAD_HOLD:
        r = s->pad_hold;
        break;

    case A_RTC_CNTL_DIG_PAD_HOLD:
        r = s->dig_pad_hold;
        break;

    case A_RTC_CNTL_EXT_WAKEUP1:
        r = s->ext_wakeup1;
        break;

    case A_RTC_CNTL_DATE:
        r = s->date_reg;
        break;

    default:
        /* Deterministic unsupported behavior for unmodeled RTC_CNTL offsets. */
        break;
    }
    return r;
}

static void esp32s3_rtc_cntl_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(opaque);
    switch (addr) {
    case A_RTC_CNTL_OPTIONS0:
        if (value & R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK) {
            s->reset_cause[0] = ESP32_SW_SYS_RESET;
            s->reset_cause[1] = ESP32_SW_SYS_RESET;
            qemu_irq_pulse(s->dig_reset_req);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK);
        }
        if (value & R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK) {
            s->reset_cause[1] = ESP32_SW_CPU_RESET;
            qemu_irq_pulse(s->cpu_reset_req[1]);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK);
        }
        if (value & R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK) {
            s->reset_cause[0] = ESP32_SW_CPU_RESET;
            qemu_irq_pulse(s->cpu_reset_req[0]);
            value &= ~(R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK);
        }
        s->options0_reg = value;
        esp32s3_rtc_update_cpu_stall(s);
        break;

    case A_RTC_CNTL_TIME_UPDATE:
        if (value & R_RTC_CNTL_TIME_UPDATE_UPDATE_MASK) {
            s->time_reg = muldiv64(
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->time_base_ns,
                s->rtc_slowclk_freq, NANOSECONDS_PER_SECOND);
        }
        break;

    case A_RTC_CNTL_SLP_TIMER0:
        s->slp_timer0 = (uint32_t)value;
        break;

    case A_RTC_CNTL_SLP_TIMER1:
        s->slp_timer1 = (uint32_t)value &
                        (R_RTC_CNTL_SLP_TIMER1_MAIN_TIMER_ALARM_EN_MASK |
                         R_RTC_CNTL_SLP_TIMER1_SLP_VAL_HI_MASK);
        break;

    case A_RTC_CNTL_RESET_STATE:
        s->stat_vector_sel[0] = FIELD_EX32(value, RTC_CNTL_RESET_STATE,
                                           PROCPU_STAT_VECTOR_SEL);
        s->stat_vector_sel[1] = FIELD_EX32(value, RTC_CNTL_RESET_STATE,
                                           APPCPU_STAT_VECTOR_SEL);
        break;

    case A_RTC_CNTL_STORE0:
    case A_RTC_CNTL_STORE1:
    case A_RTC_CNTL_STORE2:
    case A_RTC_CNTL_STORE3:
        s->scratch_reg[(addr - A_RTC_CNTL_STORE0) / 4] = value;
        break;

    case A_RTC_CNTL_WAKEUP_STATE:
        s->wakeup_state = (uint32_t)value &
                          R_RTC_CNTL_WAKEUP_STATE_WAKEUP_ENA_MASK;
        break;

    case A_RTC_CNTL_EXT_WAKEUP_CONF:
        s->ext_wakeup_conf = (uint32_t)value &
                             (R_RTC_CNTL_EXT_WAKEUP_CONF_EXT_WAKEUP1_LV_MASK |
                              R_RTC_CNTL_EXT_WAKEUP_CONF_EXT_WAKEUP0_LV_MASK |
                              R_RTC_CNTL_EXT_WAKEUP_CONF_GPIO_WAKEUP_FILTER_MASK);
        break;

    case A_RTC_CNTL_SLP_REJECT_CONF:
        s->slp_reject_conf = (uint32_t)value &
                             (R_RTC_CNTL_SLP_REJECT_CONF_DEEP_SLP_REJECT_EN_MASK |
                              R_RTC_CNTL_SLP_REJECT_CONF_LIGHT_SLP_REJECT_EN_MASK |
                              R_RTC_CNTL_SLP_REJECT_CONF_SLEEP_REJECT_ENA_MASK);
        break;

    case A_RTC_CNTL_CLK_CONF:
        s->soc_clk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, SOC_CLK_SEL);
        s->rtc_fastclk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL);
        s->rtc_slowclk = FIELD_EX32(value, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL);
        esp32s3_rtc_update_clk(s);
        break;

    case A_RTC_CNTL_SW_CPU_STALL:
        s->sw_cpu_stall_reg = value;
        esp32s3_rtc_update_cpu_stall(s);
        break;

    case A_RTC_CNTL_STORE4:
    case A_RTC_CNTL_STORE5:
    case A_RTC_CNTL_STORE6:
    case A_RTC_CNTL_STORE7:
        s->scratch_reg[(addr - A_RTC_CNTL_STORE4) / 4 + 4] = value;
        break;

    case A_RTC_CNTL_STATE0: {
        bool sleep_en = FIELD_EX32(value, RTC_CNTL_STATE0, SLEEP_EN);
        bool reject_clr = FIELD_EX32(value, RTC_CNTL_STATE0,
                                     SLP_REJECT_CAUSE_CLR);
        if (reject_clr && s->sleep_state == ESP32S3_RTC_SLEEP_REJECTED) {
            esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_AWAKE);
        }
        if (sleep_en) {
            esp32s3_rtc_enter_sleep(s);
        }
        break;
    }

    case A_RTC_CNTL_INT_CLR:
        s->int_raw &= ~(uint32_t)value;
        break;

    case A_RTC_CNTL_WDTWPROTECT:
        s->wdt_wprotect = (uint32_t)value;
        break;

    case A_RTC_CNTL_SWD_CONF:
        s->swd_conf = (uint32_t)value &
                      (R_RTC_CNTL_SWD_CONF_SWD_AUTO_FEED_EN_MASK |
                       R_RTC_CNTL_SWD_CONF_SWD_DISABLE_MASK);
        break;

    case A_RTC_CNTL_SWD_WPROTECT:
        s->swd_wprotect = (uint32_t)value;
        break;

    case A_RTC_CNTL_PAD_HOLD:
        s->pad_hold = (uint32_t)value & R_RTC_CNTL_PAD_HOLD_PAD_HOLD_MASK;
        break;

    case A_RTC_CNTL_DIG_PAD_HOLD:
        s->dig_pad_hold = (uint32_t)value;
        break;

    case A_RTC_CNTL_EXT_WAKEUP1:
        s->ext_wakeup1 = (uint32_t)value &
                         R_RTC_CNTL_EXT_WAKEUP1_EXT_WAKEUP1_SEL_MASK;
        break;

    case A_RTC_CNTL_DATE:
        s->date_reg = (uint32_t)value & R_RTC_CNTL_DATE_DATE_MASK;
        break;

    default:
        /* Deterministic unsupported behavior for unmodeled RTC_CNTL offsets. */
        break;
    }
}

static void esp32s3_rtc_update_cpu_stall(Esp32s3RtcCntlState* s)
{
    uint32_t procpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, PROCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_PROCPU_C0));

    uint32_t appcpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, APPCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_APPCPU_C0));

    const uint32_t stall_magic_val = 0x86;

    s->cpu_stall_state[0] = procpu_stall == stall_magic_val;
    s->cpu_stall_state[1] = appcpu_stall == stall_magic_val;

    qemu_set_irq(s->cpu_stall_req[0], s->cpu_stall_state[0]);
    qemu_set_irq(s->cpu_stall_req[1], s->cpu_stall_state[1]);
}

static void esp32s3_rtc_update_clk(Esp32s3RtcCntlState* s)
{
    const uint32_t slowclk_freq[] = {150000, 32768, 8000000/256};
    const uint32_t fastclk_freq[] = {s->xtal_apb_freq / 4, 8000000};
    s->rtc_slowclk_freq = slowclk_freq[s->rtc_slowclk];
    s->rtc_fastclk_freq = fastclk_freq[s->rtc_fastclk];

    qemu_irq_pulse(s->clk_update);
}

static const MemoryRegionOps esp32s3_rtc_cntl_ops = {
    .read =  esp32s3_rtc_cntl_read,
    .write = esp32s3_rtc_cntl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_rtc_cntl_reset_hold(Object *obj, ResetType type)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);

    s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(&s->slp_timer);
    esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_AWAKE);
}

static void esp32s3_rtc_cntl_realize(DeviceState *dev, Error **errp)
{
}

static void esp32s3_rtc_cntl_init(Object *obj)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_rtc_cntl_ops, s,
                          TYPE_ESP32S3_RTC_CNTL, ESP32S3_RTC_CNTL_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->dig_reset_req, ESP32S3_RTC_DIG_RESET_GPIO, 1);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->cpu_reset_req[0], ESP32S3_RTC_CPU_RESET_GPIO, ESP32S3_CPU_COUNT);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->cpu_stall_req[0], ESP32S3_RTC_CPU_STALL_GPIO, ESP32S3_CPU_COUNT);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->clk_update, ESP32S3_RTC_CLK_UPDATE_GPIO, 1);
    qdev_init_gpio_out_named(DEVICE(sbd), &s->light_sleep_req,
                             ESP32S3_RTC_LIGHT_SLEEP_GPIO, 1);

    for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
        s->reset_cause[i] = ESP32_POWERON_RESET;
        s->stat_vector_sel[i] = true;
    }

    s->rtc_slowclk = ESP32_SLOW_CLK_RC;
    s->rtc_fastclk = ESP32_FAST_CLK_8M;
    s->soc_clk = ESP32_SOC_CLK_XTAL;
    s->sleep_state = ESP32S3_RTC_SLEEP_AWAKE;
    s->date_reg = ESP32S3_RTC_CNTL_DATE_RESET;
    s->xtal_apb_freq = 40000000;
    s->pll_apb_freq = 80000000;
    esp32s3_rtc_update_clk(s);

    timer_init_ns(&s->slp_timer, QEMU_CLOCK_VIRTUAL,
                  esp32s3_rtc_slp_timer_cb, s);
}

static Property esp32s3_rtc_cntl_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_rtc_cntl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_rtc_cntl_reset_hold;
    dc->realize = esp32s3_rtc_cntl_realize;
    device_class_set_props(dc, esp32s3_rtc_cntl_properties);
}

static const TypeInfo esp32s3_rtc_cntl_info = {
    .name = TYPE_ESP32S3_RTC_CNTL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3RtcCntlState),
    .instance_init = esp32s3_rtc_cntl_init,
    .class_init = esp32s3_rtc_cntl_class_init
};

static void esp32s3_rtc_cntl_register_types(void)
{
    type_register_static(&esp32s3_rtc_cntl_info);
}

type_init(esp32s3_rtc_cntl_register_types)
