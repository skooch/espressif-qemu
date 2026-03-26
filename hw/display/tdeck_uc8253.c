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
                qemu_log_mask(LOG_GUEST_ERROR,
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
        if (s->current_cmd == UC8253_CMD_DTM2 && s->data_idx == 0) {
        }
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

static void tdeck_uc8253_init(Object *obj)
{
    TdeckUc8253State *s = TDECK_UC8253(obj);

    s->con = graphic_console_init(DEVICE(s), 0, &tdeck_uc8253_gfx_ops, s);
    qemu_console_resize(s->con, UC8253_WIDTH, UC8253_HEIGHT);

    s->busy_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                  tdeck_uc8253_busy_cb, s);

    qdev_init_gpio_out_named(DEVICE(s), &s->busy_pin, "busy", 1);
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
