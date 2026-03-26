/*
 * T-Deck Pro UC8253 EPD display model
 *
 * Emulates the UC8253 e-paper display controller on the T-Deck Pro.
 * Receives SPI data from the GP-SPI model via tdeck_uc8253_spi_receive(),
 * processes DTM1/DTM2 framebuffer commands, and renders 1bpp content to
 * a QEMU GraphicConsole as 32bpp XRGB.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/display/tdeck_uc8253.h"

#define UC8253_BUSY_POWER_MS   10
#define UC8253_BUSY_REFRESH_MS 200

/* Timer callback: deassert BUSY (set HIGH = ready) */
static void tdeck_uc8253_busy_cb(void *opaque)
{
    TdeckUc8253State *s = opaque;
    qemu_set_irq(s->busy_pin, 1);
}

/* Assert BUSY LOW and schedule deassert after delay_ms */
static void tdeck_uc8253_assert_busy(TdeckUc8253State *s, uint32_t delay_ms)
{
    qemu_set_irq(s->busy_pin, 0);
    timer_mod(s->busy_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + delay_ms);
}

/* Expand 1bpp framebuffer to 32bpp XRGB on the GraphicConsole surface */
static void tdeck_uc8253_render(TdeckUc8253State *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }

    uint32_t *pixels = (uint32_t *)surface_data(surface);
    int idx = 0;

    for (int row = 0; row < UC8253_HEIGHT; row++) {
        for (int col_byte = 0; col_byte < UC8253_WIDTH / 8; col_byte++) {
            uint8_t byte = s->current[row * (UC8253_WIDTH / 8) + col_byte];
            /* MSB first within each byte */
            for (int bit = 7; bit >= 0; bit--) {
                pixels[idx++] = (byte & (1 << bit)) ? 0x00FFFFFF : 0x00000000;
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, UC8253_WIDTH, UC8253_HEIGHT);
}

/* Handle data bytes for the current command */
static void tdeck_uc8253_data(TdeckUc8253State *s, const uint8_t *data,
                               uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        switch (s->current_cmd) {
        case UC8253_CMD_DTM1:
            if (s->data_idx < UC8253_BUF_SIZE) {
                s->previous[s->data_idx] = data[i];
            }
            s->data_idx++;
            break;

        case UC8253_CMD_DTM2:
            if (s->partial_mode) {
                /* Map partial window data_idx into absolute framebuffer position */
                uint16_t pw_width_px = s->partial_x_end - s->partial_x_start + 1;
                uint16_t pw_bytes = pw_width_px / 8;
                if (pw_bytes > 0) {
                    uint16_t row = s->data_idx / pw_bytes;
                    uint16_t col_byte = s->data_idx % pw_bytes;
                    uint16_t abs_row = s->partial_y_start + row;
                    uint16_t abs_col_byte = s->partial_x_start / 8 + col_byte;
                    if (abs_row < UC8253_HEIGHT &&
                        abs_col_byte < UC8253_WIDTH / 8) {
                        s->current[abs_row * (UC8253_WIDTH / 8) + abs_col_byte] =
                            data[i];
                    }
                }
            } else {
                if (s->data_idx < UC8253_BUF_SIZE) {
                    s->current[s->data_idx] = data[i];
                }
            }
            s->data_idx++;
            break;

        case UC8253_CMD_PARTIAL_WIN:
            if (s->data_idx < 7) {
                s->pw_buf[s->data_idx] = data[i];
            }
            s->data_idx++;
            if (s->data_idx == 7) {
                s->partial_x_start = s->pw_buf[0];
                s->partial_x_end = s->pw_buf[1];
                s->partial_y_start =
                    (uint16_t)(s->pw_buf[2] << 8) | s->pw_buf[3];
                s->partial_y_end =
                    (uint16_t)(s->pw_buf[4] << 8) | s->pw_buf[5];
                qemu_log_mask(LOG_TRACE,
                    "UC8253: partial window x=%u-%u y=%u-%u\n",
                    s->partial_x_start, s->partial_x_end,
                    s->partial_y_start, s->partial_y_end);
            }
            break;

        default:
            /* Silently consume data for unhandled commands */
            s->data_idx++;
            break;
        }
    }
}

/* Handle a command byte */
static void tdeck_uc8253_command(TdeckUc8253State *s, uint8_t cmd)
{
    s->current_cmd = cmd;
    s->data_idx = 0;

    switch (cmd) {
    case UC8253_CMD_REFRESH:
        tdeck_uc8253_render(s);
        tdeck_uc8253_assert_busy(s, UC8253_BUSY_REFRESH_MS);
        break;

    case UC8253_CMD_POWER_ON:
        s->power_on = true;
        tdeck_uc8253_assert_busy(s, UC8253_BUSY_POWER_MS);
        break;

    case UC8253_CMD_POWER_OFF:
        s->power_on = false;
        tdeck_uc8253_assert_busy(s, UC8253_BUSY_POWER_MS);
        break;

    case UC8253_CMD_PARTIAL_IN:
        s->partial_mode = true;
        break;

    case UC8253_CMD_PARTIAL_OUT:
        s->partial_mode = false;
        break;

    default:
        /* Commands with a data phase handled in tdeck_uc8253_data */
        break;
    }
}

/* Entry point called by GP-SPI model */
void tdeck_uc8253_spi_receive(TdeckUc8253State *s, const uint8_t *data,
                               uint32_t len, bool dc_level)
{
    if (!s || len == 0) {
        return;
    }

    if (!dc_level) {
        /* DC LOW = command byte */
        tdeck_uc8253_command(s, data[0]);
    } else {
        /* DC HIGH = data bytes for current command */
        tdeck_uc8253_data(s, data, len);
    }
}

/* GraphicHwOps: updates are event-driven from spi_receive, not polled */
static void tdeck_uc8253_gfx_update(void *opaque)
{
    /* No-op: rendering happens on UC8253_CMD_REFRESH */
}

static const GraphicHwOps tdeck_uc8253_gfx_ops = {
    .gfx_update = tdeck_uc8253_gfx_update,
};

/* ======================================================================= */
/* QEMU window input → T-Deck keyboard and touch                           */
/* ======================================================================= */

/* External injection APIs from tdeck_i2c_devices.c */
void tdeck_tca8418_inject_key(TdeckTca8418State *s,
                               uint8_t raw_code, bool pressed);
void tdeck_cst328_inject_touch(TdeckCst328State *s,
                                uint16_t x, uint16_t y, bool pressed);

/*
 * Map QEMU Q_KEY_CODE to TCA8418 raw key code.
 * Returns 0 for unmapped keys.
 *
 * The T-Deck has a 4x10 matrix with reversed columns:
 *   raw_code = row*10 + (9-col) + 1
 *
 * Row 0: q w e r t y u i o p
 * Row 1: a s d f g h j k l BSP
 * Row 2: ALT z x c v b n m $ ENT
 * Row 3: SHF MIC SPACE...    SYM SHF
 */
static uint8_t qcode_to_tca8418(int qcode)
{
    /*
     * Map QEMU Q_KEY_CODE to TCA8418 raw key code.
     * Q_KEY_CODEs follow QWERTY layout order (not alphabetical!).
     * TCA8418 raw code = row*10 + (9-col) + 1 (reversed columns).
     *
     * T-Deck layout:
     *   Row 0: q w e r t y u i o p
     *   Row 1: a s d f g h j k l BSP
     *   Row 2: ALT z x c v b n m $ ENT
     *   Row 3: SHF MIC SPACE...    SYM SHF
     */
#define R0(col) ((0 * 10) + (9 - (col)) + 1)
#define R1(col) ((1 * 10) + (9 - (col)) + 1)
#define R2(col) ((2 * 10) + (9 - (col)) + 1)
#define R3(col) ((3 * 10) + (9 - (col)) + 1)

    switch (qcode) {
    /* Row 0: q w e r t y u i o p */
    case Q_KEY_CODE_Q: return R0(0);
    case Q_KEY_CODE_W: return R0(1);
    case Q_KEY_CODE_E: return R0(2);
    case Q_KEY_CODE_R: return R0(3);
    case Q_KEY_CODE_T: return R0(4);
    case Q_KEY_CODE_Y: return R0(5);
    case Q_KEY_CODE_U: return R0(6);
    case Q_KEY_CODE_I: return R0(7);
    case Q_KEY_CODE_O: return R0(8);
    case Q_KEY_CODE_P: return R0(9);
    /* Row 1: a s d f g h j k l BSP */
    case Q_KEY_CODE_A: return R1(0);
    case Q_KEY_CODE_S: return R1(1);
    case Q_KEY_CODE_D: return R1(2);
    case Q_KEY_CODE_F: return R1(3);
    case Q_KEY_CODE_G: return R1(4);
    case Q_KEY_CODE_H: return R1(5);
    case Q_KEY_CODE_J: return R1(6);
    case Q_KEY_CODE_K: return R1(7);
    case Q_KEY_CODE_L: return R1(8);
    case Q_KEY_CODE_BACKSPACE: return R1(9);
    /* Row 2: ALT z x c v b n m $ ENT */
    case Q_KEY_CODE_Z: return R2(1);
    case Q_KEY_CODE_X: return R2(2);
    case Q_KEY_CODE_C: return R2(3);
    case Q_KEY_CODE_V: return R2(4);
    case Q_KEY_CODE_B: return R2(5);
    case Q_KEY_CODE_N: return R2(6);
    case Q_KEY_CODE_M: return R2(7);
    case Q_KEY_CODE_RET: return R2(9);
    /* Row 3: SHF MIC SPACE SYM SHF */
    case Q_KEY_CODE_SPC: return R3(2);
    /* Modifiers */
    case Q_KEY_CODE_SHIFT:        return R3(0);  /* Left Shift */
    case Q_KEY_CODE_SHIFT_R:      return R3(9);  /* Right Shift */
    case Q_KEY_CODE_TAB:          return R3(8);  /* SYM (symbol layer) */
    case Q_KEY_CODE_ALT:          return R2(0);  /* ALT (reserved) */
    /* Special keys */
    case Q_KEY_CODE_GRAVE_ACCENT: return R2(8);  /* $ key */
    case Q_KEY_CODE_F1:           return R3(1);  /* MIC key */
    default:
        return 0;
    }
#undef R0
#undef R1
#undef R2
#undef R3
}

static void tdeck_input_event(DeviceState *dev, QemuConsole *src,
                               InputEvent *evt)
{
    TdeckUc8253State *s = TDECK_UC8253(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY: {
        InputKeyEvent *key = evt->u.key.data;
        if (s->kbd) {
            int qcode = qemu_input_key_value_to_qcode(key->key);
            uint8_t raw = qcode_to_tca8418(qcode);
            if (raw != 0) {
                tdeck_tca8418_inject_key(s->kbd, raw, key->down);
            }
        }
        break;
    }
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *move = evt->u.abs.data;
        /* Track absolute mouse position (0-32767 range) */
        if (move->axis == INPUT_AXIS_X) {
            s->mouse_x = move->value * UC8253_WIDTH / INPUT_EVENT_ABS_MAX;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->mouse_y = move->value * UC8253_HEIGHT / INPUT_EVENT_ABS_MAX;
        }
        /* Send continuous touch updates while dragging (for swipe gestures) */
        if (s->mouse_pressed && s->touch) {
            tdeck_cst328_inject_touch(s->touch,
                                      s->mouse_x, s->mouse_y, true);
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_LEFT && s->touch) {
            s->mouse_pressed = btn->down;
            tdeck_cst328_inject_touch(s->touch,
                                      s->mouse_x, s->mouse_y,
                                      btn->down);
        }
        break;
    }
    default:
        break;
    }
}

static void tdeck_input_sync(DeviceState *dev)
{
    /* No batching needed -- events are processed immediately */
}

static const QemuInputHandler tdeck_input_handler = {
    .name  = "T-Deck Pro",
    .mask  = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = tdeck_input_event,
    .sync  = tdeck_input_sync,
};

static void tdeck_uc8253_init(Object *obj)
{
    TdeckUc8253State *s = TDECK_UC8253(obj);

    s->con = graphic_console_init(DEVICE(s), 0, &tdeck_uc8253_gfx_ops, s);
    qemu_console_resize(s->con, UC8253_WIDTH, UC8253_HEIGHT);

    s->busy_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                  tdeck_uc8253_busy_cb, s);

    qdev_init_gpio_out_named(DEVICE(s), &s->busy_pin, "busy", 1);

    /* Register input handler for keyboard and mouse from the QEMU window */
    s->input_handler = qemu_input_handler_register(DEVICE(s),
                                                    &tdeck_input_handler);
    qemu_input_handler_activate(s->input_handler);
}

static void tdeck_uc8253_reset_hold(Object *obj, ResetType type)
{
    TdeckUc8253State *s = TDECK_UC8253(obj);

    /* Both buffers start all-white (0xFF) */
    memset(s->previous, 0xFF, UC8253_BUF_SIZE);
    memset(s->current, 0xFF, UC8253_BUF_SIZE);

    s->current_cmd = 0;
    s->data_idx = 0;
    s->power_on = false;
    s->partial_mode = false;
    s->partial_x_start = 0;
    s->partial_x_end = 0;
    s->partial_y_start = 0;
    s->partial_y_end = 0;

    /* BUSY starts HIGH (ready) */
    qemu_set_irq(s->busy_pin, 1);

    /* Render initial white screen */
    tdeck_uc8253_render(s);
}

static void tdeck_uc8253_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.hold = tdeck_uc8253_reset_hold;
}

static const TypeInfo tdeck_uc8253_info = {
    .name = TYPE_TDECK_UC8253,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TdeckUc8253State),
    .instance_init = tdeck_uc8253_init,
    .class_init = tdeck_uc8253_class_init,
};

static void tdeck_uc8253_register_types(void)
{
    type_register_static(&tdeck_uc8253_info);
}

type_init(tdeck_uc8253_register_types)
