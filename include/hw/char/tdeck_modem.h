/*
 * T-Deck Pro AT Modem Simulator (A7682E)
 *
 * Chardev backend that intercepts UART1 and responds to AT commands
 * from the firmware cellular driver.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_TDECK_MODEM_H
#define HW_CHAR_TDECK_MODEM_H

#include "chardev/char.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_CHARDEV_TDECK_MODEM "chardev-tdeck-modem"

#define MODEM_CMD_BUF_SIZE  256
#define MODEM_SMS_MAX       20
#define MODEM_SMS_BODY_LEN  160
#define MODEM_SMS_SENDER_LEN 20

typedef enum {
    MODEM_CALL_IDLE,
    MODEM_CALL_DIALING,
    MODEM_CALL_RINGING,
    MODEM_CALL_ACTIVE,
} ModemCallState;

typedef struct {
    bool valid;
    char sender[MODEM_SMS_SENDER_LEN];
    char body[MODEM_SMS_BODY_LEN];
    bool read;
} ModemSms;

typedef struct TdeckModemChardev TdeckModemChardev;

DECLARE_INSTANCE_CHECKER(TdeckModemChardev, CHARDEV_TDECK_MODEM,
                         TYPE_CHARDEV_TDECK_MODEM)

struct TdeckModemChardev {
    Chardev parent;

    /* Command accumulation buffer */
    uint8_t cmd_buf[MODEM_CMD_BUF_SIZE];
    int cmd_len;

    /* SMS storage */
    ModemSms sms[MODEM_SMS_MAX];
    int sms_count;

    /* Call state */
    ModemCallState call_state;
    QEMUTimer *call_timer;

    /* Signal quality (0-31) */
    int signal_quality;

    /* SMS send state: waiting for body after > prompt */
    bool sms_input_mode;
    char sms_send_dest[MODEM_SMS_SENDER_LEN];
};

/* Public API for control panel */
void tdeck_modem_incoming_call(TdeckModemChardev *s);
void tdeck_modem_receive_sms(TdeckModemChardev *s);
void tdeck_modem_set_signal(TdeckModemChardev *s, int quality);

#endif /* HW_CHAR_TDECK_MODEM_H */
