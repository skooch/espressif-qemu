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
static void esp32s3_rtc_sync_clk_conf_sources(Esp32s3RtcCntlState *s);

#define RTC_CNTL_OPTIONS0_FORCE_MASK \
    (R_RTC_CNTL_OPTIONS0_XTL_FORCE_PU_MASK | \
     R_RTC_CNTL_OPTIONS0_BBPLL_FORCE_PU_MASK | \
     R_RTC_CNTL_OPTIONS0_BBPLL_I2C_FORCE_PU_MASK | \
     R_RTC_CNTL_OPTIONS0_BB_I2C_FORCE_PU_MASK)

#define RTC_CNTL_OPTIONS0_RW_MASK \
    (RTC_CNTL_OPTIONS0_FORCE_MASK | \
     R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK | \
     R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK | \
     R_RTC_CNTL_OPTIONS0_SW_STALL_PROCPU_C0_MASK | \
     R_RTC_CNTL_OPTIONS0_SW_STALL_APPCPU_C0_MASK)

#define RTC_CNTL_TIMER2_RW_MASK \
    R_RTC_CNTL_TIMER2_ULPCP_TOUCH_START_WAIT_MASK

#define RTC_CNTL_CLK_CONF_RW_MASK \
    (R_RTC_CNTL_CLK_CONF_ANA_CLK_RTC_SEL_MASK | \
     R_RTC_CNTL_CLK_CONF_FAST_CLK_RTC_SEL_MASK | \
     R_RTC_CNTL_CLK_CONF_XTAL_GLOBAL_FORCE_NOGATING_MASK | \
     R_RTC_CNTL_CLK_CONF_XTAL_GLOBAL_FORCE_GATING_MASK | \
     R_RTC_CNTL_CLK_CONF_CK8M_FORCE_PU_MASK | \
     R_RTC_CNTL_CLK_CONF_CK8M_FORCE_PD_MASK | \
     R_RTC_CNTL_CLK_CONF_CK8M_DFREQ_MASK | \
     R_RTC_CNTL_CLK_CONF_CK8M_FORCE_NOGATING_MASK | \
     R_RTC_CNTL_CLK_CONF_XTAL_FORCE_NOGATING_MASK | \
     R_RTC_CNTL_CLK_CONF_CK8M_DIV_SEL_MASK | \
     R_RTC_CNTL_CLK_CONF_DIG_CLK8M_EN_MASK | \
     R_RTC_CNTL_CLK_CONF_DIG_CLK8M_D256_EN_MASK | \
     R_RTC_CNTL_CLK_CONF_DIG_XTAL32K_EN_MASK | \
     R_RTC_CNTL_CLK_CONF_ENB_CK8M_DIV_MASK | \
     R_RTC_CNTL_CLK_CONF_ENB_CK8M_MASK | \
     R_RTC_CNTL_CLK_CONF_CK8M_DIV_MASK | \
     R_RTC_CNTL_CLK_CONF_CK8M_DIV_SEL_VLD_MASK | \
     R_RTC_CNTL_CLK_CONF_EFUSE_CLK_FORCE_NOGATING_MASK | \
     R_RTC_CNTL_CLK_CONF_EFUSE_CLK_FORCE_GATING_MASK)

#define RTC_CNTL_SDIO_CONF_RW_MASK \
    (R_RTC_CNTL_SDIO_CONF_XPD_SDIO_REG_MASK | \
     R_RTC_CNTL_SDIO_CONF_DREFH_SDIO_MASK | \
     R_RTC_CNTL_SDIO_CONF_DREFM_SDIO_MASK | \
     R_RTC_CNTL_SDIO_CONF_DREFL_SDIO_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_TIEH_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_FORCE_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_PD_EN_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_ENCURLIM_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_MODECURLIM_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_DCURLIM_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_EN_INITI_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_INITI_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_DCAP_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_DTHDRV_MASK | \
     R_RTC_CNTL_SDIO_CONF_SDIO_TIMER_TARGET_MASK)

#define RTC_CNTL_RTC_RW_MASK \
    R_RTC_CNTL_RTC_REGULATOR_FORCE_PU_MASK

#define RTC_CNTL_PWC_RW_MASK \
    (R_RTC_CNTL_PWC_PAD_FORCE_HOLD_MASK | \
     R_RTC_CNTL_PWC_PD_EN_MASK | \
     R_RTC_CNTL_PWC_FORCE_PU_MASK | \
     R_RTC_CNTL_PWC_FORCE_PD_MASK | \
     R_RTC_CNTL_PWC_SLOWMEM_FORCE_LPU_MASK | \
     R_RTC_CNTL_PWC_FASTMEM_FORCE_LPU_MASK)

#define RTC_CNTL_BIAS_CONF_RW_MASK \
    (R_RTC_CNTL_BIAS_CONF_DBG_ATTEN_WAKEUP_MASK | \
     R_RTC_CNTL_BIAS_CONF_DBG_ATTEN_MONITOR_MASK | \
     R_RTC_CNTL_BIAS_CONF_DBG_ATTEN_DEEP_SLP_MASK | \
     R_RTC_CNTL_BIAS_CONF_BIAS_SLEEP_MONITOR_MASK | \
     R_RTC_CNTL_BIAS_CONF_BIAS_SLEEP_DEEP_SLP_MASK | \
     R_RTC_CNTL_BIAS_CONF_PD_CUR_MONITOR_MASK | \
     R_RTC_CNTL_BIAS_CONF_PD_CUR_DEEP_SLP_MASK)

#define RTC_CNTL_REGULATOR_DRV_CTRL_RW_MASK \
    R_RTC_CNTL_REGULATOR_DRV_CTRL_DG_VDD_DRV_B_SLP_MASK

#define RTC_CNTL_DIG_PWC_RW_MASK \
    (R_RTC_CNTL_DIG_PWC_DG_WRAP_PD_EN_MASK | \
     R_RTC_CNTL_DIG_PWC_WIFI_PD_EN_MASK | \
     R_RTC_CNTL_DIG_PWC_CPU_TOP_PD_EN_MASK | \
     R_RTC_CNTL_DIG_PWC_DG_PERI_PD_EN_MASK | \
     R_RTC_CNTL_DIG_PWC_WIFI_FORCE_PU_MASK | \
     R_RTC_CNTL_DIG_PWC_LSLP_MEM_FORCE_PU_MASK)

static uint32_t esp32s3_rtc_options0_default(void)
{
    return R_RTC_CNTL_OPTIONS0_XTL_FORCE_PU_MASK;
}

static uint32_t esp32s3_rtc_timer2_default(void)
{
    return FIELD_DP32(0, RTC_CNTL_TIMER2, ULPCP_TOUCH_START_WAIT, 0x10);
}

static uint32_t esp32s3_rtc_sdio_conf_default(void)
{
    uint32_t sdio_conf = 0;

    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, DREFM_SDIO, 1);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, DREFL_SDIO, 1);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_TIEH, 1);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_PD_EN, 1);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_ENCURLIM, 1);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_EN_INITI, 1);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_INITI, 1);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_DCAP, 3);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_DTHDRV, 3);
    sdio_conf = FIELD_DP32(sdio_conf, RTC_CNTL_SDIO_CONF, SDIO_TIMER_TARGET,
                           10);

    return sdio_conf;
}

static uint32_t esp32s3_rtc_clk_conf_default(void)
{
    uint32_t clk_conf = 0;

    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL,
                          ESP32_SLOW_CLK_RC);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL,
                          ESP32_FAST_CLK_XTALD4);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF,
                          XTAL_GLOBAL_FORCE_NOGATING, 1);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF, CK8M_DFREQ, 172);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF, CK8M_DIV_SEL, 3);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF, DIG_CLK8M_D256_EN, 1);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF, CK8M_DIV, 1);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF, CK8M_DIV_SEL_VLD, 1);
    clk_conf = FIELD_DP32(clk_conf, RTC_CNTL_CLK_CONF,
                          EFUSE_CLK_FORCE_NOGATING, 1);

    return clk_conf;
}

static uint32_t esp32s3_rtc_reg_default(void)
{
    return R_RTC_CNTL_RTC_REGULATOR_FORCE_PU_MASK;
}

static uint32_t esp32s3_rtc_pwc_default(void)
{
    return R_RTC_CNTL_PWC_SLOWMEM_FORCE_LPU_MASK |
           R_RTC_CNTL_PWC_FASTMEM_FORCE_LPU_MASK;
}

static uint32_t esp32s3_rtc_bias_conf_default(void)
{
    return R_RTC_CNTL_BIAS_CONF_BIAS_SLEEP_DEEP_SLP_MASK;
}

static uint32_t esp32s3_rtc_dig_pwc_default(void)
{
    return R_RTC_CNTL_DIG_PWC_WIFI_FORCE_PU_MASK |
           R_RTC_CNTL_DIG_PWC_LSLP_MEM_FORCE_PU_MASK;
}

static uint64_t esp32s3_rtc_get_time(Esp32s3RtcCntlState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->time_base_ns,
                    s->rtc_slowclk_freq, NANOSECONDS_PER_SECOND);
}

static void esp32s3_rtc_capture_time(Esp32s3RtcCntlState *s)
{
    s->time_reg[1] = s->time_reg[0];
    s->time_reg[0] = esp32s3_rtc_get_time(s);
}

static void esp32s3_rtc_capture_time_if(Esp32s3RtcCntlState *s, bool enabled)
{
    if (enabled) {
        esp32s3_rtc_capture_time(s);
    }
}

static void esp32s3_rtc_cntl_reset_modeled_surface(Esp32s3RtcCntlState *s)
{
    s->options0_reg = esp32s3_rtc_options0_default();
    s->time_update_reg = 0;
    s->time_reg[0] = 0;
    s->time_reg[1] = 0;
    s->sw_cpu_stall_reg = 0;
    s->timer2_reg = esp32s3_rtc_timer2_default();
    s->clk_conf_reg = esp32s3_rtc_clk_conf_default();
    s->sdio_conf_reg = esp32s3_rtc_sdio_conf_default();
    s->rtc_reg = esp32s3_rtc_reg_default();
    s->pwc_reg = esp32s3_rtc_pwc_default();
    s->bias_conf_reg = esp32s3_rtc_bias_conf_default();
    s->regulator_drv_ctrl_reg = 0;
    s->dig_pwc_reg = esp32s3_rtc_dig_pwc_default();

    s->sleep_state = ESP32S3_RTC_SLEEP_AWAKE;
    s->slp_timer0 = 0;
    s->slp_timer1 = 0;
    s->wakeup_state = 0;
    s->slp_reject_conf = 0;
    s->ext_wakeup_conf = 0;
    s->ext_wakeup1 = 0;
    s->ext_wakeup1_status = 0;
    s->int_raw = 0;
    s->slp_wakeup_cause = 0;
    s->wdt_wprotect = 0;
    s->swd_conf = 0;
    s->swd_wprotect = 0;
    s->pad_hold = 0;
    s->dig_pad_hold = 0;
    s->date_reg = ESP32S3_RTC_CNTL_DATE_RESET;
}

/* wakeup_ena bits (at [31:15] of WAKEUP_STATE, so trigger bit N = reg bit N+15) */
#define WAKEUP_ENA_EXT1_BIT    (1 << 16)  /* ExtEvent1Trig = trigger bit 1 */

static void esp32s3_rtc_set_sleep_state(Esp32s3RtcCntlState *s,
                                        Esp32s3RtcSleepState state)
{
    bool old_sleeping = s->sleep_state == ESP32S3_RTC_SLEEP_SLEEPING;
    bool new_sleeping = state == ESP32S3_RTC_SLEEP_SLEEPING;

    s->sleep_state = state;

    esp32s3_rtc_capture_time_if(s,
                                old_sleeping != new_sleeping &&
                                (s->time_update_reg &
                                 R_RTC_CNTL_TIME_UPDATE_TIMER_XTL_OFF_MASK));

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

static uint32_t esp32s3_rtc_get_ext1_wakeup_status(Esp32s3RtcCntlState *s)
{
    uint32_t ext1_sel;
    uint32_t status = 0;
    bool wake_on_high;

    if (!s->gpio) {
        return 0;
    }

    ext1_sel = FIELD_EX32(s->ext_wakeup1, RTC_CNTL_EXT_WAKEUP1,
                          EXT_WAKEUP1_SEL);
    wake_on_high = FIELD_EX32(s->ext_wakeup_conf, RTC_CNTL_EXT_WAKEUP_CONF,
                              EXT_WAKEUP1_LV);

    for (int rtc_pin = 0; rtc_pin < 22; rtc_pin++) {
        int bank;
        int bit;
        bool pin_level;

        if (!(ext1_sel & BIT(rtc_pin))) {
            continue;
        }

        bank = rtc_pin / 32;
        bit = rtc_pin % 32;
        pin_level = (s->gpio->in_levels[bank] >> bit) & 1;
        if (wake_on_high ? pin_level : !pin_level) {
            status |= BIT(rtc_pin);
        }
    }

    return status;
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
        uint32_t ext1_status = esp32s3_rtc_get_ext1_wakeup_status(s);

        if (gpio_num < 22 && (ext1_status & BIT(gpio_num))) {
            esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_WOKE);
            s->slp_wakeup_cause = R_RTC_CNTL_SLP_WAKEUP_CAUSE_EXT1_MASK;
            s->ext_wakeup1_status = ext1_status;
            s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK;
            timer_del(&s->slp_timer);
            return;
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
        uint32_t ext1_status = esp32s3_rtc_get_ext1_wakeup_status(s);

        if (ext1_status != 0) {
            /* Pin is already at wake level — immediate wake, not reject.
             * This is the normal case: TCA8418 holds INT low when events
             * are pending, and the firmware disables GPIO interrupt before
             * configuring EXT1 so no race. */
            esp32s3_rtc_set_sleep_state(s, ESP32S3_RTC_SLEEP_WOKE);
            s->slp_wakeup_cause = R_RTC_CNTL_SLP_WAKEUP_CAUSE_EXT1_MASK;
            s->ext_wakeup1_status = ext1_status;
            s->int_raw |= R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK;
            return;
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
    case A_RTC_CNTL_TIMER2:
        r = s->timer2_reg;
        break;
    case A_RTC_CNTL_SDIO_CONF:
        r = s->sdio_conf_reg;
        break;
    case A_RTC_CNTL_TIME_UPDATE:
        r = s->time_update_reg | R_RTC_CNTL_TIME_UPDATE_VALID_MASK;
        break;
    case A_RTC_CNTL_TIME0:
        r = s->time_reg[0] & UINT32_MAX;
        break;
    case A_RTC_CNTL_TIME1:
        r = s->time_reg[0] >> 32;
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
        r = s->clk_conf_reg;
        break;

    case A_RTC_CNTL_RTC:
        r = s->rtc_reg;
        break;

    case A_RTC_CNTL_PWC:
        r = s->pwc_reg;
        break;

    case A_RTC_CNTL_BIAS_CONF:
        r = s->bias_conf_reg;
        break;

    case A_RTC_CNTL_REGULATOR_DRV_CTRL:
        r = s->regulator_drv_ctrl_reg;
        break;

    case A_RTC_CNTL_DIG_PWC:
        r = s->dig_pwc_reg;
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

    case A_RTC_CNTL_TIME_LOW1:
        r = s->time_reg[1] & UINT32_MAX;
        break;

    case A_RTC_CNTL_TIME_HIGH1:
        r = s->time_reg[1] >> 32;
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

    case A_RTC_CNTL_EXT_WAKEUP1_STATUS:
        r = s->ext_wakeup1_status;
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
        s->options0_reg = (uint32_t)value & RTC_CNTL_OPTIONS0_RW_MASK;
        esp32s3_rtc_update_cpu_stall(s);
        break;

    case A_RTC_CNTL_TIMER2:
        s->timer2_reg = (uint32_t)value & RTC_CNTL_TIMER2_RW_MASK;
        break;

    case A_RTC_CNTL_SDIO_CONF:
        s->sdio_conf_reg = (uint32_t)value & RTC_CNTL_SDIO_CONF_RW_MASK;
        break;

    case A_RTC_CNTL_TIME_UPDATE:
        s->time_update_reg = (uint32_t)value &
                             (R_RTC_CNTL_TIME_UPDATE_TIMER_SYS_RST_MASK |
                              R_RTC_CNTL_TIME_UPDATE_TIMER_XTL_OFF_MASK |
                              R_RTC_CNTL_TIME_UPDATE_TIMER_SYS_STALL_MASK);
        if (value & R_RTC_CNTL_TIME_UPDATE_UPDATE_MASK) {
            esp32s3_rtc_capture_time(s);
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
        s->clk_conf_reg = (uint32_t)value & RTC_CNTL_CLK_CONF_RW_MASK;
        esp32s3_rtc_sync_clk_conf_sources(s);
        esp32s3_rtc_update_clk(s);
        break;

    case A_RTC_CNTL_RTC:
        s->rtc_reg = (uint32_t)value & RTC_CNTL_RTC_RW_MASK;
        break;

    case A_RTC_CNTL_PWC:
        s->pwc_reg = (uint32_t)value & RTC_CNTL_PWC_RW_MASK;
        break;

    case A_RTC_CNTL_BIAS_CONF:
        s->bias_conf_reg = (uint32_t)value & RTC_CNTL_BIAS_CONF_RW_MASK;
        break;

    case A_RTC_CNTL_REGULATOR_DRV_CTRL:
        s->regulator_drv_ctrl_reg =
            (uint32_t)value & RTC_CNTL_REGULATOR_DRV_CTRL_RW_MASK;
        break;

    case A_RTC_CNTL_DIG_PWC:
        s->dig_pwc_reg = (uint32_t)value & RTC_CNTL_DIG_PWC_RW_MASK;
        break;

    case A_RTC_CNTL_SW_CPU_STALL:
        s->sw_cpu_stall_reg = (uint32_t)value;
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
        if (FIELD_EX32(value, RTC_CNTL_EXT_WAKEUP1, EXT_WAKEUP1_STATUS_CLR)) {
            s->ext_wakeup1_status = 0;
        }
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
    bool old_stall_state[ESP32S3_CPU_COUNT];
    uint32_t procpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, PROCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_PROCPU_C0));

    uint32_t appcpu_stall = (FIELD_EX32(s->sw_cpu_stall_reg, RTC_CNTL_SW_CPU_STALL, APPCPU_C1) << 2) |
                            (FIELD_EX32(s->options0_reg, RTC_CNTL_OPTIONS0, SW_STALL_APPCPU_C0));

    const uint32_t stall_magic_val = 0x86;

    memcpy(old_stall_state, s->cpu_stall_state, sizeof(old_stall_state));

    s->cpu_stall_state[0] = procpu_stall == stall_magic_val;
    s->cpu_stall_state[1] = appcpu_stall == stall_magic_val;

    esp32s3_rtc_capture_time_if(s,
                                (old_stall_state[0] != s->cpu_stall_state[0] ||
                                 old_stall_state[1] != s->cpu_stall_state[1]) &&
                                (s->time_update_reg &
                                 R_RTC_CNTL_TIME_UPDATE_TIMER_SYS_STALL_MASK));

    qemu_set_irq(s->cpu_stall_req[0], s->cpu_stall_state[0]);
    qemu_set_irq(s->cpu_stall_req[1], s->cpu_stall_state[1]);
}

static void esp32s3_rtc_update_clk(Esp32s3RtcCntlState* s)
{
    const uint32_t slowclk_freq[] = {150000, 32768, 8000000/256};
    const uint32_t fastclk_freq[] = {s->xtal_apb_freq / 4, 8000000};

    s->rtc_slowclk_freq = slowclk_freq[s->rtc_slowclk];
    s->rtc_fastclk_freq = fastclk_freq[s->rtc_fastclk];
}

static void esp32s3_rtc_sync_clk_conf_sources(Esp32s3RtcCntlState *s)
{
    s->rtc_fastclk = FIELD_EX32(s->clk_conf_reg, RTC_CNTL_CLK_CONF,
                                FAST_CLK_RTC_SEL);
    s->rtc_slowclk = FIELD_EX32(s->clk_conf_reg, RTC_CNTL_CLK_CONF,
                                ANA_CLK_RTC_SEL);
}

void esp32s3_rtc_notify_system_reset(Esp32s3RtcCntlState *s)
{
    esp32s3_rtc_capture_time_if(s, s->time_update_reg &
                                   R_RTC_CNTL_TIME_UPDATE_TIMER_SYS_RST_MASK);
}

static const MemoryRegionOps esp32s3_rtc_cntl_ops = {
    .read =  esp32s3_rtc_cntl_read,
    .write = esp32s3_rtc_cntl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_rtc_cntl_reset_hold(Object *obj, ResetType type)
{
    Esp32s3RtcCntlState *s = ESP32S3_RTC_CNTL(obj);

    esp32s3_rtc_cntl_reset_modeled_surface(s);
    s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(&s->slp_timer);
    esp32s3_rtc_update_cpu_stall(s);
    esp32s3_rtc_sync_clk_conf_sources(s);
    esp32s3_rtc_update_clk(s);
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
    qdev_init_gpio_out_named(DEVICE(sbd), &s->light_sleep_req,
                             ESP32S3_RTC_LIGHT_SLEEP_GPIO, 1);

    for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
        s->reset_cause[i] = ESP32_POWERON_RESET;
        s->stat_vector_sel[i] = true;
    }

    s->xtal_apb_freq = 40000000;
    s->pll_apb_freq = 80000000;
    esp32s3_rtc_cntl_reset_modeled_surface(s);
    esp32s3_rtc_update_cpu_stall(s);
    esp32s3_rtc_sync_clk_conf_sources(s);
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
