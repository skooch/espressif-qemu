/*
 * T-Deck Pro GPS Simulator (u-blox MIA-M10Q)
 *
 * Chardev backend that intercepts UART2 and emits enough UBX traffic for the
 * firmware GPS stack to configure the module, observe a module version, and
 * consume a steady navigation fix in QEMU.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "chardev/char.h"
#include "hw/char/tdeck_gps.h"

#define UBX_SYNC1 0xB5
#define UBX_SYNC2 0x62

#define UBX_CLASS_NAV 0x01
#define UBX_CLASS_ACK 0x05
#define UBX_CLASS_CFG 0x06
#define UBX_CLASS_MON 0x0A

#define UBX_ID_NAV_PVT 0x07
#define UBX_ID_NAV_SAT 0x35
#define UBX_ID_ACK_NAK 0x00
#define UBX_ID_ACK_ACK 0x01
#define UBX_ID_CFG_RST 0x04
#define UBX_ID_CFG_VALSET 0x8A
#define UBX_ID_MON_VER 0x04

#define UBX_CFG_KEY_MSGOUT_UBX_NAV_PVT_UART1 0x20910007u
#define UBX_CFG_KEY_MSGOUT_UBX_NAV_SAT_UART1 0x20910016u
#define UBX_CFG_KEY_UART1OUTPROT_NMEA        0x10740002u
#define UBX_CFG_KEY_RATE_MEAS                0x30210001u
#define UBX_CFG_KEY_RATE_NAV                 0x30210002u
#define UBX_CFG_KEY_PM_OPERATEMODE           0x20D00001u
#define UBX_CFG_KEY_SIGNAL_GPS_ENA           0x10310001u
#define UBX_CFG_KEY_SIGNAL_GAL_ENA           0x10310007u
#define UBX_CFG_KEY_SIGNAL_BDS_ENA           0x1031000Du
#define UBX_CFG_KEY_SIGNAL_GLO_ENA           0x10310004u

#define GPS_SW_VERSION "QEMU-M10Q 1.0"
#define GPS_HW_VERSION "QEMU"

static uint16_t ubx_le16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t ubx_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static void ubx_put_le16(uint8_t *buf, uint16_t value)
{
    buf[0] = value & 0xff;
    buf[1] = value >> 8;
}

static void ubx_put_le32(uint8_t *buf, uint32_t value)
{
    buf[0] = value & 0xff;
    buf[1] = (value >> 8) & 0xff;
    buf[2] = (value >> 16) & 0xff;
    buf[3] = (value >> 24) & 0xff;
}

static void ubx_put_le_i32(uint8_t *buf, int32_t value)
{
    ubx_put_le32(buf, (uint32_t)value);
}

static void gps_checksum(const uint8_t *buf, size_t len, uint8_t *ck_a, uint8_t *ck_b)
{
    uint8_t a = 0;
    uint8_t b = 0;

    for (size_t i = 0; i < len; i++) {
        a = a + buf[i];
        b = b + a;
    }

    *ck_a = a;
    *ck_b = b;
}

static void gps_send_frame(TdeckGpsChardev *s, uint8_t cls, uint8_t id,
                           const uint8_t *payload, size_t payload_len)
{
    Chardev *chr = CHARDEV(s);
    uint8_t frame[6 + 400 + 2];
    uint8_t ck_a;
    uint8_t ck_b;
    size_t total_len = 6 + payload_len + 2;

    g_assert(payload_len <= 400);

    frame[0] = UBX_SYNC1;
    frame[1] = UBX_SYNC2;
    frame[2] = cls;
    frame[3] = id;
    ubx_put_le16(&frame[4], payload_len);
    if (payload_len > 0) {
        memcpy(&frame[6], payload, payload_len);
    }
    gps_checksum(&frame[2], 4 + payload_len, &ck_a, &ck_b);
    frame[6 + payload_len] = ck_a;
    frame[7 + payload_len] = ck_b;

    qemu_chr_be_write(chr, frame, total_len);
}

static void gps_send_ack(TdeckGpsChardev *s, bool ack, uint8_t cls_id, uint8_t msg_id)
{
    uint8_t payload[2] = { cls_id, msg_id };
    gps_send_frame(s, UBX_CLASS_ACK, ack ? UBX_ID_ACK_ACK : UBX_ID_ACK_NAK,
                   payload, sizeof(payload));
}

static void gps_send_mon_ver(TdeckGpsChardev *s)
{
    uint8_t payload[40];

    memset(payload, 0, sizeof(payload));
    memcpy(&payload[0], GPS_SW_VERSION,
           MIN(strlen(GPS_SW_VERSION), (size_t)30));
    memcpy(&payload[30], GPS_HW_VERSION,
           MIN(strlen(GPS_HW_VERSION), (size_t)10));

    gps_send_frame(s, UBX_CLASS_MON, UBX_ID_MON_VER, payload, sizeof(payload));
}

static void gps_send_nav_pvt(TdeckGpsChardev *s)
{
    uint8_t payload[92];
    uint32_t step = s->epoch;
    uint32_t total_seconds = step;
    uint8_t sec = total_seconds % 60;
    uint8_t min = (total_seconds / 60) % 60;
    uint8_t hour = (12 + (total_seconds / 3600)) % 24;
    int32_t lat = -378136290;
    int32_t lon = 1449630580 + (int32_t)(step * 120);

    memset(payload, 0, sizeof(payload));

    ubx_put_le32(&payload[0], step * 1000);
    ubx_put_le16(&payload[4], 2026);
    payload[6] = 4;
    payload[7] = 11;
    payload[8] = hour;
    payload[9] = min;
    payload[10] = sec;
    payload[11] = 0x03;
    ubx_put_le32(&payload[12], 50000);
    ubx_put_le_i32(&payload[16], 0);
    payload[20] = 3;
    payload[21] = 0x01;
    payload[22] = 0x00;
    payload[23] = 8;
    ubx_put_le_i32(&payload[24], lon);
    ubx_put_le_i32(&payload[28], lat);
    ubx_put_le_i32(&payload[32], 34000);
    ubx_put_le_i32(&payload[36], 32000);
    ubx_put_le32(&payload[40], 2500);
    ubx_put_le32(&payload[44], 3500);
    ubx_put_le_i32(&payload[48], 0);
    ubx_put_le_i32(&payload[52], 1200);
    ubx_put_le_i32(&payload[56], 0);
    ubx_put_le_i32(&payload[60], 1200);
    ubx_put_le_i32(&payload[64], 9000000);
    ubx_put_le32(&payload[68], 300);
    ubx_put_le32(&payload[72], 120000);
    ubx_put_le16(&payload[76], 150);

    gps_send_frame(s, UBX_CLASS_NAV, UBX_ID_NAV_PVT, payload, sizeof(payload));
}

static void gps_send_nav_sat(TdeckGpsChardev *s)
{
    uint8_t payload[8 + (8 * 12)];

    memset(payload, 0, sizeof(payload));
    ubx_put_le32(&payload[0], s->epoch * 1000);
    payload[4] = 1;
    payload[5] = 8;

    for (int i = 0; i < 8; i++) {
        uint8_t *sat = &payload[8 + (i * 12)];

        sat[0] = (i < 4) ? 0 : 2;
        sat[1] = 10 + i;
        sat[2] = 32 + i;
        sat[3] = 30 + i;
        ubx_put_le16(&sat[4], 45 * i);
        ubx_put_le16(&sat[6], 0);
        ubx_put_le32(&sat[8], (1u << 3));
    }

    gps_send_frame(s, UBX_CLASS_NAV, UBX_ID_NAV_SAT, payload, sizeof(payload));
}

static void gps_emit_nav_samples(TdeckGpsChardev *s, bool force_all)
{
    if (s->nav_pvt_rate > 0 &&
        (force_all || (s->epoch % s->nav_pvt_rate) == 0)) {
        gps_send_nav_pvt(s);
    }
    if (s->nav_sat_rate > 0 &&
        (force_all || (s->epoch % s->nav_sat_rate) == 0)) {
        gps_send_nav_sat(s);
    }
}

static uint32_t gps_frame_interval_ms(const TdeckGpsChardev *s)
{
    uint32_t meas_ms = s->meas_period_ms ? s->meas_period_ms : 1000;
    uint32_t nav_rate = s->nav_rate ? s->nav_rate : 1;

    return meas_ms * nav_rate;
}

static bool gps_streaming_enabled(const TdeckGpsChardev *s)
{
    return s->powered && (s->nav_pvt_rate > 0 || s->nav_sat_rate > 0);
}

static void gps_schedule_timer(TdeckGpsChardev *s)
{
    if (!gps_streaming_enabled(s)) {
        timer_del(s->nav_timer);
        return;
    }

    timer_mod(s->nav_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)gps_frame_interval_ms(s) * SCALE_MS);
}

static void gps_reset_config(TdeckGpsChardev *s)
{
    s->rx_len = 0;
    s->epoch = 0;
    s->meas_period_ms = 1000;
    s->nav_rate = 1;
    s->nav_pvt_rate = 0;
    s->nav_sat_rate = 0;
    s->nmea_enabled = false;
    timer_del(s->nav_timer);
}

static size_t gps_cfg_value_size(uint32_t key_id)
{
    switch (key_id) {
    case UBX_CFG_KEY_RATE_MEAS:
    case UBX_CFG_KEY_RATE_NAV:
        return 2;
    case UBX_CFG_KEY_MSGOUT_UBX_NAV_PVT_UART1:
    case UBX_CFG_KEY_MSGOUT_UBX_NAV_SAT_UART1:
    case UBX_CFG_KEY_UART1OUTPROT_NMEA:
    case UBX_CFG_KEY_PM_OPERATEMODE:
    case UBX_CFG_KEY_SIGNAL_GPS_ENA:
    case UBX_CFG_KEY_SIGNAL_GAL_ENA:
    case UBX_CFG_KEY_SIGNAL_BDS_ENA:
    case UBX_CFG_KEY_SIGNAL_GLO_ENA:
        return 1;
    default:
        return 0;
    }
}

static void gps_apply_valset(TdeckGpsChardev *s, const uint8_t *payload, size_t payload_len)
{
    size_t off = 4;

    if (payload_len < 4) {
        return;
    }

    while (off + 4 <= payload_len) {
        uint32_t key_id = ubx_le32(&payload[off]);
        size_t value_size = gps_cfg_value_size(key_id);

        off += 4;
        if (value_size == 0 || off + value_size > payload_len) {
            break;
        }

        switch (key_id) {
        case UBX_CFG_KEY_MSGOUT_UBX_NAV_PVT_UART1:
            s->nav_pvt_rate = payload[off];
            break;
        case UBX_CFG_KEY_MSGOUT_UBX_NAV_SAT_UART1:
            s->nav_sat_rate = payload[off];
            break;
        case UBX_CFG_KEY_UART1OUTPROT_NMEA:
            s->nmea_enabled = payload[off] != 0;
            break;
        case UBX_CFG_KEY_RATE_MEAS:
            s->meas_period_ms = ubx_le16(&payload[off]);
            break;
        case UBX_CFG_KEY_RATE_NAV:
            s->nav_rate = ubx_le16(&payload[off]);
            break;
        default:
            break;
        }

        off += value_size;
    }

    gps_schedule_timer(s);
}

static void gps_process_frame(TdeckGpsChardev *s, uint8_t cls, uint8_t id,
                              const uint8_t *payload, size_t payload_len)
{
    bool was_streaming = gps_streaming_enabled(s);

    if (!s->powered) {
        return;
    }

    if (cls == UBX_CLASS_CFG && id == UBX_ID_CFG_VALSET) {
        gps_apply_valset(s, payload, payload_len);
        gps_send_ack(s, true, cls, id);
        if (!was_streaming && gps_streaming_enabled(s)) {
            s->epoch = 1;
            gps_emit_nav_samples(s, true);
        }
        return;
    }

    if (cls == UBX_CLASS_CFG && id == UBX_ID_CFG_RST) {
        s->epoch = 0;
        gps_send_ack(s, true, cls, id);
        gps_schedule_timer(s);
        return;
    }

    if (cls == UBX_CLASS_MON && id == UBX_ID_MON_VER && payload_len == 0) {
        gps_send_mon_ver(s);
        return;
    }

    if (cls == UBX_CLASS_CFG) {
        gps_send_ack(s, false, cls, id);
    }
}

static void gps_consume_rx(TdeckGpsChardev *s)
{
    while (s->rx_len >= 8) {
        uint16_t payload_len;
        size_t frame_len;
        uint8_t ck_a;
        uint8_t ck_b;
        uint8_t cls;
        uint8_t id;

        if (s->rx_buf[0] != UBX_SYNC1 || s->rx_buf[1] != UBX_SYNC2) {
            size_t drop = 1;

            while (drop < s->rx_len && s->rx_buf[drop] != UBX_SYNC1) {
                drop++;
            }
            memmove(s->rx_buf, s->rx_buf + drop, s->rx_len - drop);
            s->rx_len -= drop;
            continue;
        }

        payload_len = ubx_le16(&s->rx_buf[4]);
        frame_len = 6 + payload_len + 2;
        if (frame_len > sizeof(s->rx_buf)) {
            s->rx_len = 0;
            return;
        }
        if (s->rx_len < frame_len) {
            return;
        }

        gps_checksum(&s->rx_buf[2], 4 + payload_len, &ck_a, &ck_b);
        if (s->rx_buf[6 + payload_len] == ck_a &&
            s->rx_buf[7 + payload_len] == ck_b) {
            cls = s->rx_buf[2];
            id = s->rx_buf[3];
            gps_process_frame(s, cls, id, &s->rx_buf[6], payload_len);
        }

        memmove(s->rx_buf, s->rx_buf + frame_len, s->rx_len - frame_len);
        s->rx_len -= frame_len;
    }
}

static void gps_nav_timer_cb(void *opaque)
{
    TdeckGpsChardev *s = CHARDEV_TDECK_GPS(opaque);

    if (!gps_streaming_enabled(s)) {
        return;
    }

    s->epoch++;
    gps_emit_nav_samples(s, false);

    gps_schedule_timer(s);
}

static int tdeck_gps_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    TdeckGpsChardev *s = CHARDEV_TDECK_GPS(chr);
    size_t available;
    size_t copy_len;

    if (!s->powered) {
        return len;
    }

    available = sizeof(s->rx_buf) - s->rx_len;
    copy_len = MIN((size_t)len, available);
    if (copy_len == 0) {
        s->rx_len = 0;
        return len;
    }

    memcpy(s->rx_buf + s->rx_len, buf, copy_len);
    s->rx_len += copy_len;
    gps_consume_rx(s);

    return len;
}

static void tdeck_gps_chr_open(Chardev *chr,
                               ChardevBackend *backend,
                               bool *be_opened,
                               Error **errp)
{
    *be_opened = true;
}

void tdeck_gps_gpio_en(TdeckGpsChardev *s, int level)
{
    bool new_powered = level != 0;

    if (new_powered == s->powered) {
        return;
    }

    s->powered = new_powered;
    if (!s->powered) {
        gps_reset_config(s);
        return;
    }

    gps_reset_config(s);
    s->powered = true;
}

static void tdeck_gps_instance_init(Object *obj)
{
    TdeckGpsChardev *s = CHARDEV_TDECK_GPS(obj);

    s->powered = false;
    s->nav_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gps_nav_timer_cb, s);
    gps_reset_config(s);
}

static void tdeck_gps_instance_finalize(Object *obj)
{
    TdeckGpsChardev *s = CHARDEV_TDECK_GPS(obj);

    timer_free(s->nav_timer);
}

static void tdeck_gps_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_write = tdeck_gps_chr_write;
    cc->open = tdeck_gps_chr_open;
}

static const TypeInfo tdeck_gps_type_info = {
    .name = TYPE_CHARDEV_TDECK_GPS,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(TdeckGpsChardev),
    .instance_init = tdeck_gps_instance_init,
    .instance_finalize = tdeck_gps_instance_finalize,
    .class_init = tdeck_gps_class_init,
};

static void register_types(void)
{
    type_register_static(&tdeck_gps_type_info);
}

type_init(register_types);
