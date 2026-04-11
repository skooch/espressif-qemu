#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "hw/char/tdeck_gps.h"

static bool quit;

typedef struct GpsFeHandler {
    int read_count;
    uint8_t read_buf[512];
} GpsFeHandler;

static void main_loop_until_read(void)
{
    quit = false;
    do {
        main_loop_wait(false);
    } while (!quit);
}

static int gps_fe_can_read(void *opaque)
{
    GpsFeHandler *h = opaque;

    return sizeof(h->read_buf) - h->read_count;
}

static void gps_fe_read(void *opaque, const uint8_t *buf, int size)
{
    GpsFeHandler *h = opaque;

    g_assert_cmpint(size, <=, gps_fe_can_read(opaque));
    memcpy(h->read_buf + h->read_count, buf, size);
    h->read_count += size;
    quit = true;
}

static void gps_wait_for_bytes(GpsFeHandler *h)
{
    if (h->read_count == 0) {
        main_loop_until_read();
    }
    g_assert_cmpint(h->read_count, >, 0);
}

static size_t gps_write_header(uint8_t *buf, uint8_t cls, uint8_t id, uint16_t payload_len)
{
    buf[0] = 0xB5;
    buf[1] = 0x62;
    buf[2] = cls;
    buf[3] = id;
    buf[4] = payload_len & 0xff;
    buf[5] = payload_len >> 8;
    return 6;
}

static size_t gps_append_checksum(uint8_t *buf, size_t end)
{
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;

    for (size_t i = 2; i < end; i++) {
        ck_a = ck_a + buf[i];
        ck_b = ck_b + ck_a;
    }

    buf[end] = ck_a;
    buf[end + 1] = ck_b;
    return end + 2;
}

static size_t gps_make_poll(uint8_t *buf, uint8_t cls, uint8_t id)
{
    return gps_append_checksum(buf, gps_write_header(buf, cls, id, 0));
}

static size_t gps_make_cfg_valset(uint8_t *buf)
{
    size_t off = gps_write_header(buf, 0x06, 0x8A, 4 + 5 + 6 + 6);

    buf[off++] = 0x01;
    buf[off++] = 0x01;
    buf[off++] = 0x00;
    buf[off++] = 0x00;

    buf[off++] = 0x07;
    buf[off++] = 0x00;
    buf[off++] = 0x91;
    buf[off++] = 0x20;
    buf[off++] = 0x01;

    buf[off++] = 0x01;
    buf[off++] = 0x00;
    buf[off++] = 0x21;
    buf[off++] = 0x30;
    buf[off++] = 0xE8;
    buf[off++] = 0x03;

    buf[off++] = 0x02;
    buf[off++] = 0x00;
    buf[off++] = 0x21;
    buf[off++] = 0x30;
    buf[off++] = 0x01;
    buf[off++] = 0x00;

    return gps_append_checksum(buf, off);
}

static size_t gps_parse_frame(const uint8_t *buf, size_t buf_len,
                              uint8_t expected_cls, uint8_t expected_id)
{
    uint16_t payload_len;
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    size_t total_len;

    g_assert_cmpint(buf_len, >=, 8);
    g_assert_cmpuint(buf[0], ==, 0xB5);
    g_assert_cmpuint(buf[1], ==, 0x62);
    g_assert_cmpuint(buf[2], ==, expected_cls);
    g_assert_cmpuint(buf[3], ==, expected_id);

    payload_len = buf[4] | ((uint16_t)buf[5] << 8);
    total_len = 6 + payload_len + 2;
    g_assert_cmpint(buf_len, >=, total_len);

    for (size_t i = 2; i < 6 + payload_len; i++) {
        ck_a = ck_a + buf[i];
        ck_b = ck_b + ck_a;
    }
    g_assert_cmpuint(buf[6 + payload_len], ==, ck_a);
    g_assert_cmpuint(buf[7 + payload_len], ==, ck_b);

    return total_len;
}

static void char_tdeck_gps_test(void)
{
    Chardev *chr;
    CharBackend be;
    GpsFeHandler h = { 0 };
    uint8_t cmd[64];
    size_t len;
    size_t frame_len;

    chr = qemu_chardev_new("tdeck-gps-test", TYPE_CHARDEV_TDECK_GPS,
                           NULL, NULL, &error_abort);
    g_assert_nonnull(chr);

    qemu_chr_fe_init(&be, chr, &error_abort);
    qemu_chr_fe_set_handlers(&be, gps_fe_can_read, gps_fe_read,
                             NULL, NULL, &h, NULL, true);

    len = gps_make_poll(cmd, 0x0A, 0x04);
    qemu_chr_fe_write(&be, cmd, len);
    g_assert_cmpint(h.read_count, ==, 0);

    tdeck_gps_gpio_en(CHARDEV_TDECK_GPS(chr), 1);

    qemu_chr_fe_write(&be, cmd, len);
    gps_wait_for_bytes(&h);
    frame_len = gps_parse_frame(h.read_buf, h.read_count, 0x0A, 0x04);
    g_assert_cmpint(frame_len, ==, 48);
    g_assert_cmpmem(&h.read_buf[6], 13, "QEMU-M10Q 1.0", 13);
    g_assert_cmpmem(&h.read_buf[36], 4, "QEMU", 4);

    memset(&h, 0, sizeof(h));
    len = gps_make_cfg_valset(cmd);
    qemu_chr_fe_write(&be, cmd, len);
    gps_wait_for_bytes(&h);

    frame_len = gps_parse_frame(h.read_buf, h.read_count, 0x05, 0x01);
    g_assert_cmpint(h.read_buf[6], ==, 0x06);
    g_assert_cmpint(h.read_buf[7], ==, 0x8A);

    gps_parse_frame(h.read_buf + frame_len, h.read_count - frame_len, 0x01, 0x07);
    g_assert_cmpuint(h.read_buf[frame_len + 26], ==, 0x03);
    g_assert_cmpuint(h.read_buf[frame_len + 27] & 0x01, ==, 0x01);
    g_assert_cmpuint(h.read_buf[frame_len + 29], ==, 8);

    tdeck_gps_gpio_en(CHARDEV_TDECK_GPS(chr), 0);
    memset(&h, 0, sizeof(h));
    qemu_chr_fe_write(&be, cmd, len);
    g_assert_cmpint(h.read_count, ==, 0);

    qemu_chr_fe_deinit(&be, true);
    object_unparent(OBJECT(chr));
}

int main(int argc, char **argv)
{
    qemu_init_main_loop(&error_abort);
    g_test_init(&argc, &argv, NULL);

    module_call_init(MODULE_INIT_QOM);

    g_test_add_func("/char/tdeck-gps", char_tdeck_gps_test);

    return g_test_run();
}
