/*
 * T-Deck Pro AT Modem Simulator (A7682E)
 *
 * Chardev backend that intercepts UART1 and responds to AT commands.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include "hw/char/tdeck_modem.h"

/* Send a raw string back to the UART RX path */
static void modem_send(TdeckModemChardev *s, const char *str)
{
    Chardev *chr = CHARDEV(s);
    int len = strlen(str);
    qemu_chr_be_write(chr, (const uint8_t *)str, len);
}

/* Send an AT response wrapped in CRLF */
static void modem_respond(TdeckModemChardev *s, const char *response)
{
    modem_send(s, "\r\n");
    modem_send(s, response);
    modem_send(s, "\r\n");
}

/* Send a multi-part response (e.g., "+CSQ: 20,0" then "OK") */
static void modem_respond_ok(TdeckModemChardev *s, const char *prefix)
{
    if (prefix) {
        modem_respond(s, prefix);
    }
    modem_respond(s, "OK");
}

/* Timer callback for dial connect transition */
static void modem_call_connect_cb(void *opaque)
{
    TdeckModemChardev *s = CHARDEV_TDECK_MODEM(opaque);
    if (s->call_state == MODEM_CALL_DIALING) {
        s->call_state = MODEM_CALL_ACTIVE;
    }
}

/* Find next free SMS slot, returns index or -1 */
static int modem_sms_alloc(TdeckModemChardev *s)
{
    for (int i = 0; i < MODEM_SMS_MAX; i++) {
        if (!s->sms[i].valid) {
            s->sms_count++;
            return i;
        }
    }
    return -1;
}

/* Process a complete AT command line (without trailing CR) */
static void modem_process_cmd(TdeckModemChardev *s, const char *cmd)
{
    char buf[256];

    /* Skip leading whitespace */
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }

    /* Empty line */
    if (*cmd == '\0') {
        return;
    }

    /* Must start with AT (case-insensitive) */
    if (g_ascii_strncasecmp(cmd, "AT", 2) != 0) {
        modem_respond(s, "ERROR");
        return;
    }

    /* "AT" alone */
    if (cmd[2] == '\0') {
        modem_respond(s, "OK");
        return;
    }

    const char *rest = cmd + 2;

    /* ATE0 - disable echo */
    if (g_ascii_strcasecmp(rest, "E0") == 0 ||
        g_ascii_strcasecmp(rest, "E1") == 0) {
        modem_respond(s, "OK");
        return;
    }

    /* AT+CPIN? */
    if (g_ascii_strcasecmp(rest, "+CPIN?") == 0) {
        modem_respond_ok(s, "+CPIN: READY");
        return;
    }

    /* AT+CSQ */
    if (g_ascii_strcasecmp(rest, "+CSQ") == 0) {
        snprintf(buf, sizeof(buf), "+CSQ: %d,0", s->signal_quality);
        modem_respond_ok(s, buf);
        return;
    }

    /* AT+CREG? */
    if (g_ascii_strcasecmp(rest, "+CREG?") == 0) {
        modem_respond_ok(s, "+CREG: 0,1");
        return;
    }

    /* AT+CGREG? */
    if (g_ascii_strcasecmp(rest, "+CGREG?") == 0) {
        modem_respond_ok(s, "+CGREG: 0,1");
        return;
    }

    /* AT+COPS? */
    if (g_ascii_strcasecmp(rest, "+COPS?") == 0) {
        modem_respond_ok(s, "+COPS: 0,0,\"QEMU Mobile\"");
        return;
    }

    /* AT+CNSMOD? */
    if (g_ascii_strcasecmp(rest, "+CNSMOD?") == 0) {
        modem_respond_ok(s, "+CNSMOD: 0,7");
        return;
    }

    /* AT+CLCC - list current calls */
    if (g_ascii_strcasecmp(rest, "+CLCC") == 0) {
        if (s->call_state == MODEM_CALL_ACTIVE) {
            modem_respond_ok(s, "+CLCC: 1,0,0,0,0,\"+1555000000\",145");
        } else if (s->call_state == MODEM_CALL_DIALING) {
            modem_respond_ok(s, "+CLCC: 1,0,2,0,0,\"+1555000000\",145");
        } else if (s->call_state == MODEM_CALL_RINGING) {
            modem_respond_ok(s, "+CLCC: 1,1,4,0,0,\"+1555123456\",145");
        } else {
            modem_respond(s, "OK");
        }
        return;
    }

    /* AT+CHUP - hang up */
    if (g_ascii_strcasecmp(rest, "+CHUP") == 0) {
        s->call_state = MODEM_CALL_IDLE;
        timer_del(s->call_timer);
        modem_respond(s, "OK");
        return;
    }

    /* ATA - answer call */
    if (g_ascii_strcasecmp(rest, "A") == 0) {
        if (s->call_state == MODEM_CALL_RINGING) {
            s->call_state = MODEM_CALL_ACTIVE;
        }
        modem_respond(s, "OK");
        return;
    }

    /* ATD - dial */
    if (rest[0] == 'D' || rest[0] == 'd') {
        s->call_state = MODEM_CALL_DIALING;
        /* Schedule connect after 2 seconds */
        timer_mod(s->call_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2000000000LL);
        modem_respond(s, "OK");
        return;
    }

    /* AT+CMGL="ALL" - list SMS */
    if (g_ascii_strncasecmp(rest, "+CMGL=", 6) == 0) {
        for (int i = 0; i < MODEM_SMS_MAX; i++) {
            if (s->sms[i].valid) {
                snprintf(buf, sizeof(buf),
                         "+CMGL: %d,\"%s\",\"%s\",,\"\"\r\n%s",
                         i,
                         s->sms[i].read ? "REC READ" : "REC UNREAD",
                         s->sms[i].sender,
                         s->sms[i].body);
                modem_respond(s, buf);
            }
        }
        modem_respond(s, "OK");
        return;
    }

    /* AT+CMGR=N - read SMS */
    if (g_ascii_strncasecmp(rest, "+CMGR=", 6) == 0) {
        int idx = atoi(rest + 6);
        if (idx >= 0 && idx < MODEM_SMS_MAX && s->sms[idx].valid) {
            s->sms[idx].read = true;
            snprintf(buf, sizeof(buf),
                     "+CMGR: \"REC READ\",\"%s\",,\"\"\r\n%s",
                     s->sms[idx].sender,
                     s->sms[idx].body);
            modem_respond_ok(s, buf);
        } else {
            modem_respond(s, "OK");
        }
        return;
    }

    /* AT+CMGS="number" - send SMS (enter input mode) */
    if (g_ascii_strncasecmp(rest, "+CMGS=", 6) == 0) {
        const char *num = rest + 6;
        /* Strip quotes */
        if (*num == '"') {
            num++;
        }
        g_strlcpy(s->sms_send_dest, num, MODEM_SMS_SENDER_LEN);
        char *q = strchr(s->sms_send_dest, '"');
        if (q) {
            *q = '\0';
        }
        s->sms_input_mode = true;
        /* Send > prompt (no CRLF wrapping, just \r\n> ) */
        modem_send(s, "\r\n> ");
        return;
    }

    /* AT+CMGD=N - delete SMS */
    if (g_ascii_strncasecmp(rest, "+CMGD=", 6) == 0) {
        int idx = atoi(rest + 6);
        if (idx >= 0 && idx < MODEM_SMS_MAX && s->sms[idx].valid) {
            s->sms[idx].valid = false;
            s->sms_count--;
        }
        modem_respond(s, "OK");
        return;
    }

    /* Default: any unrecognized AT command gets OK */
    modem_respond(s, "OK");
}

static int tdeck_modem_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    TdeckModemChardev *s = CHARDEV_TDECK_MODEM(chr);

    if (s->power_state != MODEM_READY) {
        return len;
    }

    for (int i = 0; i < len; i++) {
        uint8_t c = buf[i];

        /* SMS body input mode: accumulate until Ctrl-Z (0x1A) */
        if (s->sms_input_mode) {
            if (c == 0x1A) {
                /* End of SMS body */
                s->sms_input_mode = false;
                s->cmd_buf[s->cmd_len] = '\0';
                /* We just acknowledge the send */
                char resp[32];
                snprintf(resp, sizeof(resp), "+CMGS: %d", s->sms_count);
                modem_respond_ok(s, resp);
                s->cmd_len = 0;
            } else if (s->cmd_len < MODEM_CMD_BUF_SIZE - 1) {
                s->cmd_buf[s->cmd_len++] = c;
            }
            continue;
        }

        /* Normal AT command accumulation */
        if (c == '\r' || c == '\n') {
            if (s->cmd_len > 0) {
                s->cmd_buf[s->cmd_len] = '\0';
                modem_process_cmd(s, (const char *)s->cmd_buf);
                s->cmd_len = 0;
            }
        } else if (s->cmd_len < MODEM_CMD_BUF_SIZE - 1) {
            s->cmd_buf[s->cmd_len++] = c;
        }
    }

    return len;
}

static void tdeck_modem_chr_open(Chardev *chr,
                                  ChardevBackend *backend,
                                  bool *be_opened,
                                  Error **errp)
{
    *be_opened = true;
}

/* Power state machine */

static void tdeck_modem_boot_timer_cb(void *opaque)
{
    TdeckModemChardev *s = CHARDEV_TDECK_MODEM(opaque);
    if (s->power_state == MODEM_BOOTING) {
        s->power_state = MODEM_READY;
    }
}

static void tdeck_modem_reset_state(TdeckModemChardev *s)
{
    s->cmd_len = 0;
    s->call_state = MODEM_CALL_IDLE;
    s->sms_input_mode = false;
    s->signal_quality = 20;
    /* Keep SMS store intact across power cycles for persistence */
}

void tdeck_modem_gpio_en(TdeckModemChardev *s, int level)
{
    if (level && s->power_state == MODEM_POWER_OFF) {
        s->power_state = MODEM_POWER_RAIL_ON;
    } else if (!level && s->power_state != MODEM_POWER_OFF) {
        s->power_state = MODEM_POWER_OFF;
        timer_del(s->boot_timer);
        tdeck_modem_reset_state(s);
    }
}

void tdeck_modem_gpio_pwrkey(TdeckModemChardev *s, int level)
{
    /* Detect falling edge (HIGH -> LOW) while rail is on */
    if (s->pwrkey_level && !level && s->power_state == MODEM_POWER_RAIL_ON) {
        s->power_state = MODEM_BOOTING;
        timer_mod(s->boot_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * NANOSECONDS_PER_SECOND);
    }
    s->pwrkey_level = level;
}

ModemPowerState tdeck_modem_get_power_state(TdeckModemChardev *s)
{
    return s->power_state;
}

/* Public API */

void tdeck_modem_incoming_call(TdeckModemChardev *s)
{
    if (s->power_state != MODEM_READY) {
        return;
    }
    if (s->call_state != MODEM_CALL_IDLE) {
        return;
    }
    s->call_state = MODEM_CALL_RINGING;
    modem_send(s, "\r\nRING\r\n");
    modem_send(s, "\r\n+CLIP: \"+1555123456\",145\r\n");
}

void tdeck_modem_receive_sms(TdeckModemChardev *s)
{
    if (s->power_state != MODEM_READY) {
        return;
    }
    int idx = modem_sms_alloc(s);
    if (idx < 0) {
        return;
    }
    s->sms[idx].valid = true;
    g_strlcpy(s->sms[idx].sender, "+1555987654", MODEM_SMS_SENDER_LEN);
    g_strlcpy(s->sms[idx].body, "Hello from QEMU!", MODEM_SMS_BODY_LEN);
    s->sms[idx].read = false;

    char buf[32];
    snprintf(buf, sizeof(buf), "\r\n+CMTI: \"ME\",%d\r\n", idx);
    modem_send(s, buf);
}

void tdeck_modem_set_signal(TdeckModemChardev *s, int quality)
{
    if (s->power_state != MODEM_READY) {
        return;
    }
    if (quality < 0) {
        quality = 0;
    }
    if (quality > 31) {
        quality = 31;
    }
    s->signal_quality = quality;
}

static void tdeck_modem_instance_init(Object *obj)
{
    TdeckModemChardev *s = CHARDEV_TDECK_MODEM(obj);
    s->signal_quality = 20;
    s->call_state = MODEM_CALL_IDLE;
    s->cmd_len = 0;
    s->sms_count = 0;
    s->sms_input_mode = false;
    s->call_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  modem_call_connect_cb, s);
    s->power_state = MODEM_POWER_OFF;
    s->pwrkey_level = true;  /* PWRKEY is active-low, idle HIGH */
    s->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  tdeck_modem_boot_timer_cb, s);
}

static void tdeck_modem_instance_finalize(Object *obj)
{
    TdeckModemChardev *s = CHARDEV_TDECK_MODEM(obj);
    timer_free(s->call_timer);
    timer_free(s->boot_timer);
}

static void tdeck_modem_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);
    cc->chr_write = tdeck_modem_chr_write;
    cc->open = tdeck_modem_chr_open;
}

static const TypeInfo tdeck_modem_type_info = {
    .name = TYPE_CHARDEV_TDECK_MODEM,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(TdeckModemChardev),
    .instance_init = tdeck_modem_instance_init,
    .instance_finalize = tdeck_modem_instance_finalize,
    .class_init = tdeck_modem_class_init,
};

static void register_types(void)
{
    type_register_static(&tdeck_modem_type_info);
}

type_init(register_types);
