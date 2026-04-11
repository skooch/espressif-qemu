/*
 * T-Deck Pro GPS Simulator (u-blox MIA-M10Q)
 *
 * Chardev backend that intercepts UART2 and responds to the subset of UBX
 * traffic used by the firmware GPS driver.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_TDECK_GPS_H
#define HW_CHAR_TDECK_GPS_H

#include "chardev/char.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_CHARDEV_TDECK_GPS "chardev-tdeck-gps"

#define GPS_RX_BUF_SIZE 512

typedef struct TdeckGpsChardev TdeckGpsChardev;

DECLARE_INSTANCE_CHECKER(TdeckGpsChardev, CHARDEV_TDECK_GPS,
                         TYPE_CHARDEV_TDECK_GPS)

struct TdeckGpsChardev {
    Chardev parent;

    uint8_t rx_buf[GPS_RX_BUF_SIZE];
    size_t rx_len;

    bool powered;
    QEMUTimer *nav_timer;

    uint32_t epoch;
    uint16_t meas_period_ms;
    uint16_t nav_rate;
    uint8_t nav_pvt_rate;
    uint8_t nav_sat_rate;
    bool nmea_enabled;
};

void tdeck_gps_gpio_en(TdeckGpsChardev *s, int level);

#endif /* HW_CHAR_TDECK_GPS_H */
