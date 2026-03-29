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
#include "hw/ssi/tdeck_sd_spi.h"
#include "hw/char/tdeck_modem.h"
#include "ui/vgafont.h"

#define UC8253_BUSY_POWER_MS   10

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

#define CONSOLE_WIDTH (UC8253_WIDTH + 240)

/* Panel text rendering using VGA 8x16 font */
static void panel_putchar(uint32_t *pixels, int stride, int x, int y,
                            char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = vgafont16 + (unsigned char)ch * 16;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            pixels[(y + row) * stride + x + col] =
                (bits & (0x80 >> col)) ? fg : bg;
        }
    }
}

static void panel_puts(uint32_t *pixels, int stride, int x, int y,
                         const char *text, uint32_t fg, uint32_t bg)
{
    while (*text) {
        panel_putchar(pixels, stride, x, y, *text, fg, bg);
        x += 8;
        text++;
    }
}

/* Forward declaration */
static void tdeck_panel_render(TdeckUc8253State *s);

/* External I2C register accessors from tdeck_i2c_devices.c */
void tdeck_bq27220_set_reg(TdeckBq27220State *s, uint8_t addr, uint8_t val);
void tdeck_bq25896_set_reg(TdeckBq25896State *s, uint8_t addr, uint8_t val);

/* Update BQ27220 SOC and voltage registers from panel state */
static void tdeck_panel_update_battery(TdeckUc8253State *s)
{
    if (!s->bq27220) return;
    uint16_t soc = (uint16_t)s->panel_soc;
    uint16_t mv = 3300 + (uint16_t)(s->panel_soc * 9);  /* 3300-4200mV range */
    /* BQ27220 SOC register at 0x1C (16-bit LE) */
    tdeck_bq27220_set_reg(s->bq27220, 0x1C, soc & 0xFF);
    tdeck_bq27220_set_reg(s->bq27220, 0x1D, (soc >> 8) & 0xFF);
    /* BQ27220 Voltage register at 0x08 (16-bit LE) */
    tdeck_bq27220_set_reg(s->bq27220, 0x08, mv & 0xFF);
    tdeck_bq27220_set_reg(s->bq27220, 0x09, (mv >> 8) & 0xFF);
}

/* Update BQ25896 charge status register from panel state */
static void tdeck_panel_update_charger(TdeckUc8253State *s)
{
    if (!s->bq25896) return;
    /* REG0B[7:5]=VBUS_STAT, [4:3]=CHRG_STAT */
    static const uint8_t vals[] = {
        0x00,         /* Off: no input, not charging */
        0x50,         /* Fast: USB host (010), fast charging (10) */
        0x58,         /* Done: USB host (010), charge done (11) */
    };
    tdeck_bq25896_set_reg(s->bq25896, 0x0B, vals[s->panel_charger % 3]);
}

/* Handle a mouse click in the control panel area */
static void tdeck_panel_click(TdeckUc8253State *s, int px, int py)
{
    /* Battery [-] button: around (8-32, 48-64) */
    if (px >= 8 && px <= 32 && py >= 48 && py <= 64) {
        s->panel_soc -= 5;
        if (s->panel_soc < 0) s->panel_soc = 0;
        tdeck_panel_update_battery(s);
    }
    /* Battery [+] button: around (40-64, 48-64) */
    else if (px >= 40 && px <= 64 && py >= 48 && py <= 64) {
        s->panel_soc += 5;
        if (s->panel_soc > 100) s->panel_soc = 100;
        tdeck_panel_update_battery(s);
    }
    /* Charger [Toggle]: around (8-80, 92-108) */
    else if (px >= 8 && px <= 80 && py >= 92 && py <= 108) {
        s->panel_charger = (s->panel_charger + 1) % 3;
        tdeck_panel_update_charger(s);
    }
    /* [Call] button: around (8-48, 136-152) */
    else if (px >= 8 && px <= 48 && py >= 136 && py <= 152) {
        if (s->modem && tdeck_modem_get_power_state(s->modem) == MODEM_READY) {
            tdeck_modem_incoming_call(s->modem);
        }
    }
    /* [SMS] button: around (8-48, 156-172) */
    else if (px >= 8 && px <= 48 && py >= 156 && py <= 172) {
        if (s->modem && tdeck_modem_get_power_state(s->modem) == MODEM_READY) {
            tdeck_modem_receive_sms(s->modem);
        }
    }
    /* [Signal] button: around (8-72, 176-192) */
    else if (px >= 8 && px <= 72 && py >= 176 && py <= 192) {
        if (!s->modem || tdeck_modem_get_power_state(s->modem) != MODEM_READY) {
            return;
        }
        if (s->panel_signal == 20) s->panel_signal = 10;
        else if (s->panel_signal == 10) s->panel_signal = 0;
        else s->panel_signal = 20;
        tdeck_modem_set_signal(s->modem, s->panel_signal);
    }
    /* [Eject/Insert] SD card: around (8-56, 220-236) */
    else if (px >= 8 && px <= 56 && py >= 220 && py <= 236) {
        if (s->sd_spi && s->sd_spi->sd) {
            s->panel_sd_inserted = !s->panel_sd_inserted;
            tdeck_sd_spi_set_inserted(s->sd_spi, s->panel_sd_inserted);
        }
    }
    else {
        return;  /* no button hit */
    }

    tdeck_panel_render(s);
    dpy_gfx_update(s->con, 240, 0, 240, UC8253_HEIGHT);
}

/* Render control panel on the right 240px */
static void tdeck_panel_render(TdeckUc8253State *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }

    uint32_t *pixels = (uint32_t *)surface_data(surface);
    int stride = CONSOLE_WIDTH;
    int px = 240;  /* panel x offset */

    uint32_t bg = 0x00E8E8E8;      /* light gray background */
    uint32_t fg = 0x00000000;      /* black text */
    uint32_t btn_bg = 0x00D0D0D0;  /* button background */

    /* Fill panel background */
    for (int row = 0; row < UC8253_HEIGHT; row++) {
        for (int col = px; col < CONSOLE_WIDTH; col++) {
            pixels[row * stride + col] = bg;
        }
    }

    /* Title */
    panel_puts(pixels, stride, px + 8, 4, "T-Deck Simulator", fg, bg);

    /* Divider line */
    for (int col = px; col < CONSOLE_WIDTH; col++) {
        pixels[22 * stride + col] = fg;
    }

    /* Battery section */
    char buf[32];
    panel_puts(pixels, stride, px + 8, 28, "Battery", fg, bg);
    snprintf(buf, sizeof(buf), "[-]");
    panel_puts(pixels, stride, px + 8, 48, buf, fg, btn_bg);
    snprintf(buf, sizeof(buf), "[+]");
    panel_puts(pixels, stride, px + 40, 48, buf, fg, btn_bg);
    snprintf(buf, sizeof(buf), "%d%%", s->panel_soc);
    panel_puts(pixels, stride, px + 80, 48, buf, fg, bg);

    /* Charger section */
    panel_puts(pixels, stride, px + 8, 72, "Charger", fg, bg);
    const char *charger_text[] = {"Off", "Fast", "Done"};
    snprintf(buf, sizeof(buf), "[Toggle] %s", charger_text[s->panel_charger % 3]);
    panel_puts(pixels, stride, px + 8, 92, buf, fg, btn_bg);

    /* Cellular section */
    panel_puts(pixels, stride, px + 8, 116, "Cellular", fg, bg);

    /* Modem power state */
    const char *power_str;
    switch (tdeck_modem_get_power_state(s->modem)) {
    case MODEM_POWER_OFF:     power_str = "Modem: OFF";      break;
    case MODEM_POWER_RAIL_ON: power_str = "Modem: Rail On";  break;
    case MODEM_BOOTING:       power_str = "Modem: Booting";  break;
    case MODEM_READY:         power_str = "Modem: Ready";    break;
    default:                  power_str = "Modem: ???";      break;
    }
    panel_puts(pixels, stride, px + 80, 116, power_str, fg, bg);

    uint32_t cell_btn = (tdeck_modem_get_power_state(s->modem) == MODEM_READY)
                        ? btn_bg : 0x00C0C0C0;
    panel_puts(pixels, stride, px + 8, 136, "[Call]", fg, cell_btn);
    panel_puts(pixels, stride, px + 8, 156, "[SMS]", fg, cell_btn);
    panel_puts(pixels, stride, px + 8, 176, "[Signal]", fg, cell_btn);
    snprintf(buf, sizeof(buf), "CSQ: %d", s->panel_signal);
    panel_puts(pixels, stride, px + 80, 176, buf, fg, bg);

    /* SD Card section */
    panel_puts(pixels, stride, px + 8, 200, "SD Card", fg, bg);
    if (s->sd_spi && s->sd_spi->sd) {
        const char *sd_btn = s->panel_sd_inserted ? "[Eject]" : "[Insert]";
        const char *sd_status = s->panel_sd_inserted ? "Inserted" : "Ejected";
        panel_puts(pixels, stride, px + 8, 220, sd_btn, fg, btn_bg);
        panel_puts(pixels, stride, px + 72, 220, sd_status, fg, bg);
    } else {
        panel_puts(pixels, stride, px + 8, 220, "No image", fg, bg);
    }
}

/* Expand 1bpp framebuffer to 32bpp XRGB on the GraphicConsole surface */
static void tdeck_uc8253_render(TdeckUc8253State *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }

    uint32_t *pixels = (uint32_t *)surface_data(surface);

    for (int row = 0; row < UC8253_HEIGHT; row++) {
        for (int col_byte = 0; col_byte < UC8253_WIDTH / 8; col_byte++) {
            uint8_t byte = s->current[row * (UC8253_WIDTH / 8) + col_byte];
            /* MSB first within each byte */
            for (int bit = 7; bit >= 0; bit--) {
                int col = col_byte * 8 + (7 - bit);
                pixels[row * CONSOLE_WIDTH + col] =
                    (byte & (1 << bit)) ? 0x00FFFFFF : 0x00000000;
            }
        }
    }

    tdeck_panel_render(s);
    dpy_gfx_update(s->con, 0, 0, CONSOLE_WIDTH, UC8253_HEIGHT);
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

        case 0xE5: /* ForceTemperature */
            if (s->data_idx == 0) {
                s->force_temp = data[i];
            }
            s->data_idx++;
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
    case UC8253_CMD_REFRESH: {
        tdeck_uc8253_render(s);
        int busy_ms;
        if (s->partial_mode) {
            busy_ms = 200;   /* Turbo/partial */
        } else if (s->force_temp == 0x6E) {
            busy_ms = 2000;  /* Full refresh */
        } else if (s->force_temp == 0x79) {
            busy_ms = 500;   /* Fast refresh */
        } else {
            busy_ms = 200;   /* Default */
        }
        tdeck_uc8253_assert_busy(s, busy_ms);
        break;
    }

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
        /* Track absolute mouse position (0-32767 range), scaled to 480-wide window */
        if (move->axis == INPUT_AXIS_X) {
            s->mouse_x = move->value * CONSOLE_WIDTH / INPUT_EVENT_ABS_MAX;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->mouse_y = move->value * UC8253_HEIGHT / INPUT_EVENT_ABS_MAX;
        }
        /* Send continuous touch updates while dragging (EPD area only) */
        if (s->mouse_pressed && s->touch && s->mouse_x < UC8253_WIDTH) {
            tdeck_cst328_inject_touch(s->touch,
                                      s->mouse_x, s->mouse_y, true);
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_LEFT) {
            if (s->mouse_x < UC8253_WIDTH && s->touch) {
                /* EPD area touch */
                s->mouse_pressed = btn->down;
                tdeck_cst328_inject_touch(s->touch,
                                          s->mouse_x, s->mouse_y,
                                          btn->down);
            } else if (s->mouse_x >= UC8253_WIDTH && btn->down) {
                /* Panel click -- route to control panel handler */
                tdeck_panel_click(s, s->mouse_x - UC8253_WIDTH, s->mouse_y);
            }
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
    qemu_console_resize(s->con, CONSOLE_WIDTH, UC8253_HEIGHT);

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
    s->force_temp = 0;
    s->partial_x_start = 0;
    s->partial_x_end = 0;
    s->partial_y_start = 0;
    s->partial_y_end = 0;

    /* Control panel defaults */
    s->panel_soc = 85;
    s->panel_charger = 0;
    s->panel_signal = 20;
    s->panel_sd_inserted = (s->sd_spi && s->sd_spi->inserted) ? 1 : 0;

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
