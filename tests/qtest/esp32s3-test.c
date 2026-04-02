/*
 * QTest coverage for the ESP32-S3 machine.
 *
 * This initial suite focuses on the immediate fidelity program:
 * SPI1 transfer bounds, GP-SPI completion semantics, GPIO/IO_MUX state,
 * USB Serial/JTAG RX/TX behavior, I2C completion semantics, UART clock
 * dependence, SHA IRQ behavior, and the boot-critical ANA register path.
 */

#include "qemu/osdep.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "libqtest.h"
#include "qemu/bitops.h"
#include "glib/gstdio.h"

#include "hw/char/esp32_uart.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"
#include "hw/i2c/esp32_i2c.h"
#include "hw/misc/esp32c3_jtag.h"
#include "hw/misc/esp32s3_rng.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/misc/esp_sha.h"
#include "hw/ssi/esp32s3_gpspi.h"
#include "hw/ssi/esp32s3_spi.h"
#include "hw/xtensa/esp32s3_clk.h"
#include "hw/xtensa/esp32s3_clk_defs.h"

#define ANA_BASE                0x6000e000
#define SPI2_BASE               DR_REG_SPI2_BASE
#define SPI1_BASE               DR_REG_SPI1_BASE
#define GPIO_BASE               DR_REG_GPIO_BASE
#define IOMUX_BASE              DR_REG_IO_MUX_BASE
#define JTAG_BASE               DR_REG_USB_SERIAL_JTAG_BASE
#define I2C0_BASE               DR_REG_I2C_EXT_BASE
#define UART0_BASE              DR_REG_UART_BASE
#define SHA_BASE                DR_REG_SHA_BASE
#define SYSTEM_BASE             DR_REG_SYSTEM_BASE
#define PMS_BASE                DR_REG_SENSITIVE_BASE

#define GPSPI_CMD_UPDATE_BIT    BIT(23)
#define GPSPI_CMD_USR_BIT       BIT(24)
#define GPSPI_INT_TRANS_DONE    BIT(12)
#define USB_SERIAL_JTAG_EP1_DATA_FREE        BIT(1)
#define USB_SERIAL_JTAG_EP1_WR_DONE         BIT(0)

static QTestState *qts_start(void)
{
    return qtest_init("-M esp32s3");
}

static void test_ana_pll_done(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, ANA_BASE + 0x40) & BIT(24), ==, BIT(24));

    qtest_writel(qts, ANA_BASE + 0x44, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, ANA_BASE + 0x44), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_gpio_and_iomux_state(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(34)),
                    ==, ESP32S3_GPIO_SIG_GPIO_OUT);
    g_assert_cmphex((qtest_readl(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(34)) &
                     ESP32S3_IOMUX_MCU_SEL_MASK) >> ESP32S3_IOMUX_MCU_SEL_SHIFT,
                    ==, ESP32S3_GPIO_IOMUX_FUNC_GPIO);

    qtest_writel(qts, GPIO_BASE + GPIO_STATUS_W1TS_REG, BIT(5));
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_STATUS_REG), ==, BIT(5));

    qtest_writel(qts, GPIO_BASE + GPIO_STATUS_W1TC_REG, BIT(5));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_STATUS_REG), ==, 0);

    qtest_writel(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(34), 0);
    g_assert_cmphex((qtest_readl(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(34)) &
                     ESP32S3_IOMUX_MCU_SEL_MASK) >> ESP32S3_IOMUX_MCU_SEL_SHIFT,
                    ==, ESP32S3_GPIO_IOMUX_FUNC_DIRECT);

    qtest_quit(qts);
}

static void test_gpspi_completion_irq(void)
{
    QTestState *qts = qts_start();

    qtest_writel(qts, SPI2_BASE + 0x34, GPSPI_INT_TRANS_DONE);
    qtest_writel(qts, SPI2_BASE + 0x00, GPSPI_CMD_USR_BIT | GPSPI_CMD_UPDATE_BIT);

    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x00), ==, GPSPI_CMD_USR_BIT);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x40), ==, 0);

    qtest_clock_step(qts, 700);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x00), ==, GPSPI_CMD_USR_BIT);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x40), ==, 0);

    qtest_clock_step(qts, 800);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x00), ==, GPSPI_CMD_USR_BIT);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x3c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x40), ==, 0);

    qtest_clock_step(qts, 800);

    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x00), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x3c), ==, GPSPI_INT_TRANS_DONE);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x40), ==, GPSPI_INT_TRANS_DONE);

    qtest_writel(qts, SPI2_BASE + 0x38, GPSPI_INT_TRANS_DONE);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x40), ==, 0);

    qtest_quit(qts);
}

static void test_gpio_signal_remap_state(void)
{
    QTestState *qts = qts_start();

    qtest_writel(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(34),
                 ESP32S3_GPIO_SIG_GPIO_OUT);

    qtest_writel(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(7),
                 ESP32S3_GPIO_IOMUX_FUNC_GPIO << ESP32S3_IOMUX_MCU_SEL_SHIFT);
    qtest_writel(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(7), ESP32S3_GPIO_SIG_EPD_CS);
    qtest_writel(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(34),
                 ESP32S3_GPIO_IOMUX_FUNC_DIRECT << ESP32S3_IOMUX_MCU_SEL_SHIFT);

    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(7)),
                    ==, ESP32S3_GPIO_SIG_EPD_CS);
    g_assert_cmphex((qtest_readl(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(34)) &
                     ESP32S3_IOMUX_MCU_SEL_MASK) >> ESP32S3_IOMUX_MCU_SEL_SHIFT,
                    ==, ESP32S3_GPIO_IOMUX_FUNC_DIRECT);
    g_assert_cmphex((qtest_readl(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(7)) &
                     ESP32S3_IOMUX_MCU_SEL_MASK) >> ESP32S3_IOMUX_MCU_SEL_SHIFT,
                    ==, ESP32S3_GPIO_IOMUX_FUNC_GPIO);

    qtest_quit(qts);
}

static void test_gpio_signal_non_default_routing(void)
{
    QTestState *qts = qts_start();

    qtest_writel(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(34),
                 ESP32S3_GPIO_SIG_SD_CS);
    qtest_writel(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(7),
                 ESP32S3_GPIO_SIG_EPD_CS);
    qtest_writel(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(7),
                 ESP32S3_GPIO_IOMUX_FUNC_GPIO << ESP32S3_IOMUX_MCU_SEL_SHIFT);
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(34)),
                    ==, ESP32S3_GPIO_SIG_SD_CS);
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(7)),
                    ==, ESP32S3_GPIO_SIG_EPD_CS);

    qtest_writel(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(34),
                 ESP32S3_GPIO_SIG_GPIO_OUT);
    qtest_writel(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(7),
                 ESP32S3_GPIO_IOMUX_FUNC_DIRECT << ESP32S3_IOMUX_MCU_SEL_SHIFT);
    g_assert_cmphex((qtest_readl(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(7)) &
                     ESP32S3_IOMUX_MCU_SEL_MASK) >> ESP32S3_IOMUX_MCU_SEL_SHIFT,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(7)),
                    ==, ESP32S3_GPIO_SIG_EPD_CS);

    qtest_writel(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(7),
                 ESP32S3_GPIO_SIG_GPIO_OUT);
    qtest_writel(qts, GPIO_BASE + GPIO_OUT1_REG, BIT(2));
    qtest_writel(qts, GPIO_BASE + GPIO_OUT_REG, BIT(7));
    qtest_writel(qts, GPIO_BASE + GPIO_ENABLE1_REG, BIT(2));
    qtest_writel(qts, GPIO_BASE + GPIO_ENABLE_REG, BIT(7));

    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_OUT_REG) & BIT(7), ==, BIT(7));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_OUT1_REG) & BIT(2), ==, BIT(2));
    qtest_writel(qts, GPIO_BASE + GPIO_OUT1_W1TC_REG, BIT(2));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_OUT1_REG) & BIT(2),
                    ==, 0);
    qtest_writel(qts, GPIO_BASE + GPIO_OUT_W1TC_REG, BIT(7));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_OUT_REG) & BIT(7),
                    ==, 0);

    qtest_quit(qts);
}

static void test_spi1_usr_rx_overwrites_all_bytes(void)
{
    QTestState *qts = qts_start();
    uint32_t w0;

    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_USER,
                 R_SPI_MEM_USER_USR_MOSI_MASK | R_SPI_MEM_USER_USR_MISO_MASK);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_MOSI_DLEN, 31);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_MISO_DLEN, 31);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_W0, 0xfdfe0201);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_CMD, R_SPI_MEM_CMD_USR_MASK);

    w0 = qtest_readl(qts, SPI1_BASE + A_SPI_MEM_W0);
    g_assert_cmphex(w0 >> 16, !=, 0xfdfe);

    qtest_quit(qts);
}

#ifndef _WIN32
static int connect_unix_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

    for (int i = 0; i < 200; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        g_usleep(1000);
    }

    g_error("failed to connect to %s", path);
}

static void test_usb_serial_jtag_rx_tx(void)
{
    g_autofree gchar *sock_dir = g_dir_make_tmp("esp32s3-jtag-XXXXXX", NULL);
    g_autofree gchar *sock_path = g_build_filename(sock_dir, "jtag.sock", NULL);
    QTestState *qts;
    int sock_fd;
    char tx_buf[3] = {0};
    char rx_byte = 'Z';
    char rx_block[76] = {0};
    int read_count = 0;

    qts = qtest_initf("-M esp32s3 "
                      "-serial null -serial null "
                      "-chardev socket,id=usj,path=%s,server=on,wait=off "
                      "-serial chardev:usj", sock_path);
    sock_fd = connect_unix_socket(sock_path);

    qtest_writel(qts, JTAG_BASE + USB_SERIAL_JTAG_INT_ENA_REG,
                 USB_SERIAL_JTAG_INT_RX_AVAIL | USB_SERIAL_JTAG_INT_TX_DONE);
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_CONF_REG) &
                    USB_SERIAL_JTAG_EP1_DATA_FREE, ==, USB_SERIAL_JTAG_EP1_DATA_FREE);
    qtest_writel(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_REG, 'O');
    qtest_writel(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_REG, 'K');
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_CONF_REG) &
                    USB_SERIAL_JTAG_EP1_DATA_FREE, ==, USB_SERIAL_JTAG_EP1_DATA_FREE);
    qtest_writel(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_CONF_REG, 0x1);

    g_assert_cmpint(read(sock_fd, tx_buf, 2), ==, 2);
    g_assert_cmpmem(tx_buf, 2, "OK", 2);
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_INT_RAW_REG) &
                    USB_SERIAL_JTAG_INT_TX_DONE,
                    ==, USB_SERIAL_JTAG_INT_TX_DONE);

    g_assert_cmpint(write(sock_fd, &rx_byte, 1), ==, 1);
    for (int i = 0; i < 50; i++) {
        qtest_clock_step(qts, 1000);
        if (qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_INT_RAW_REG) &
            USB_SERIAL_JTAG_INT_RX_AVAIL) {
            break;
        }
    }

    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_INT_RAW_REG) &
                    USB_SERIAL_JTAG_INT_RX_AVAIL,
                    ==, USB_SERIAL_JTAG_INT_RX_AVAIL);
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_REG), ==, 'Z');
    for (size_t i = 0; i < 64; i++) {
        rx_block[i] = (char)('A' + (i % 26));
    }
    g_assert_cmpint(write(sock_fd, rx_block, 70), ==, 70);
    read_count = 0;
    for (int i = 0; i < 80; i++) {
        qtest_clock_step(qts, 1000);
        if (qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_REG) == 0) {
            break;
        }
        read_count++;
    }
    g_assert_cmpint(read_count, ==, 64);

    for (size_t i = 0; i < 64; i++) {
        qtest_writel(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_REG, (uint8_t)'T');
    }
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_CONF_REG) &
                    USB_SERIAL_JTAG_EP1_DATA_FREE, ==, 0);
    qtest_writel(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_REG, (uint8_t)'U');
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_CONF_REG) &
                    USB_SERIAL_JTAG_EP1_DATA_FREE, ==, USB_SERIAL_JTAG_EP1_DATA_FREE);

    g_assert_cmpint(write(sock_fd, rx_block, 70), ==, 70);
    g_assert_cmpint(write(sock_fd, rx_block, 70), ==, 70);

    qtest_writel(qts, JTAG_BASE + USB_SERIAL_JTAG_INT_CLR_REG,
                 USB_SERIAL_JTAG_INT_RX_AVAIL | USB_SERIAL_JTAG_INT_TX_DONE);
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_INT_RAW_REG), ==, 0);

    close(sock_fd);
    qtest_quit(qts);
}
#endif

static uint32_t i2c_cmd_write(size_t bytes, bool ack_check)
{
    uint32_t v = FIELD_DP32(0, I2C_CMD, BYTE_NUM, bytes);
    v = FIELD_DP32(v, I2C_CMD, OPCODE, I2C_OPCODE_WRITE);
    v = FIELD_DP32(v, I2C_CMD, ACK_CHECK_EN, ack_check ? 1 : 0);
    return v;
}

static uint32_t i2c_cmd_stop(void)
{
    return FIELD_DP32(0, I2C_CMD, OPCODE, I2C_OPCODE_STOP);
}

static void i2c_start_transaction(QTestState *qts)
{
    uint32_t ctr = FIELD_DP32(0, I2C_CTR, MS_MODE, 1);
    ctr = FIELD_DP32(ctr, I2C_CTR, TRANS_START, 1);
    qtest_writel(qts, I2C0_BASE + A_I2C_CTR, ctr);
    qtest_clock_step(qts, 3000);
}

static void test_i2c_done_and_ack_semantics(void)
{
    QTestState *qts = qts_start();

    qtest_writel(qts, I2C0_BASE + A_I2C_INT_ENA,
                 R_I2C_INT_ENA_ACK_ERR_MASK | R_I2C_INT_ENA_TRANS_COMPLETE_MASK);

    /* Successful write to a present device */
    qtest_writel(qts, I2C0_BASE + A_I2C_FIFO_DATA, 0x68); /* 0x34 write */
    qtest_writel(qts, I2C0_BASE + A_I2C_FIFO_DATA, 0x00);
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD, i2c_cmd_write(2, true));
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD + 4, i2c_cmd_stop());
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD + 8, i2c_cmd_write(1, true));
    i2c_start_transaction(qts);

    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_ACK_ERR_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_TRANS_COMPLETE_MASK,
                    ==, R_I2C_INT_RAW_TRANS_COMPLETE_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_CMD) &
                    R_I2C_CMD_DONE_MASK, ==, R_I2C_CMD_DONE_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_CMD + 4) &
                    R_I2C_CMD_DONE_MASK, ==, R_I2C_CMD_DONE_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_CMD + 8) &
                    R_I2C_CMD_DONE_MASK, ==, 0);

    qtest_writel(qts, I2C0_BASE + A_I2C_INT_CLR,
                 R_I2C_INT_CLR_TRANS_COMPLETE_MASK | R_I2C_INT_CLR_ACK_ERR_MASK);

    /* NACK on an unmapped address */
    qtest_writel(qts, I2C0_BASE + A_I2C_FIFO_DATA, 0xfe); /* 0x7f write */
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD, i2c_cmd_write(1, true));
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD + 4, i2c_cmd_stop());
    i2c_start_transaction(qts);

    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_ACK_ERR_MASK,
                    ==, R_I2C_INT_RAW_ACK_ERR_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_CMD) &
                    R_I2C_CMD_DONE_MASK, ==, R_I2C_CMD_DONE_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_CMD + 4) &
                    R_I2C_CMD_DONE_MASK, ==, 0);

    qtest_quit(qts);
}

static void test_i2c_unsupported_modes(void)
{
    QTestState *qts = qts_start();
    uint32_t fifo_conf = FIELD_DP32(0, I2C_FIFO_CONF, NONFIFO_EN, 1);
    uint32_t ctr = FIELD_DP32(0, I2C_CTR, MS_MODE, 0);
    ctr = FIELD_DP32(ctr, I2C_CTR, TRANS_START, 1);

    qtest_writel(qts, I2C0_BASE + A_I2C_FIFO_CONF, fifo_conf);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_FIFO_CONF) &
                    R_I2C_FIFO_CONF_NONFIFO_EN_MASK, ==, R_I2C_FIFO_CONF_NONFIFO_EN_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_FIFO_CONF) &
                    (R_I2C_FIFO_CONF_RX_FIFO_RST_MASK |
                     R_I2C_FIFO_CONF_TX_FIFO_RST_MASK), ==, 0);

    qtest_writel(qts, I2C0_BASE + A_I2C_CTR, ctr);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_CTR) &
                    R_I2C_CTR_MS_MODE_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_CTR) &
                    R_I2C_CTR_TRANS_START_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW), ==, 0);

    qtest_quit(qts);
}

static void test_uart_clock_dependent_timing(void)
{
    QTestState *qts = qts_start();
    uint32_t default_pulse = qtest_readl(qts, UART0_BASE + A_UART_LOWPULSE);
    uint32_t sysclk_xtal = 0;

    g_assert_cmpuint(default_pulse, >, 0);

    sysclk_xtal = FIELD_DP32(sysclk_xtal, SYSTEM_SYSCLK_CONF, PRE_DIV_CNT, 1);
    sysclk_xtal = FIELD_DP32(sysclk_xtal, SYSTEM_SYSCLK_CONF, CLK_XTAL_FREQ, 40);
    sysclk_xtal = FIELD_DP32(sysclk_xtal, SYSTEM_SYSCLK_CONF, SOC_CLK_SEL,
                             ESP32S3_CLK_SEL_XTAL);
    sysclk_xtal = FIELD_DP32(sysclk_xtal, SYSTEM_SYSCLK_CONF, CLK_DIV_EN, 1);
    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_SYSCLK_CONF, sysclk_xtal);

    g_assert_cmpuint(qtest_readl(qts, UART0_BASE + A_UART_LOWPULSE), <, default_pulse);

    qtest_writel(qts, UART0_BASE + A_UART_AUTOBAUD,
                 FIELD_DP32(0, UART_AUTOBAUD, EN, 1));
    g_assert_cmpuint(qtest_readl(qts, UART0_BASE + A_UART_RXD_CNT), ==, 400);

    qtest_quit(qts);
}

static void test_sha_irq(void)
{
    QTestState *qts = qts_start();

    qtest_writel(qts, SHA_BASE + A_SHA_IRQ_ENA,
                 FIELD_DP32(0, SHA_IRQ_ENA, INTERRUPT_ENA, 1));
    qtest_writel(qts, SHA_BASE + A_SHA_MODE, ESP_SHA_256_MODE);
    qtest_writel(qts, SHA_BASE + A_SHA_M_MEM, 0x61626380);
    qtest_writel(qts, SHA_BASE + A_SHA_START,
                 FIELD_DP32(0, SHA_START, START, 1));

    g_assert_cmphex(qtest_readl(qts, SHA_BASE + A_SHA_BUSY), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SHA_BASE + A_SHA_H_MEM), !=, 0);

    qtest_writel(qts, SHA_BASE + A_SHA_CLEAR_IRQ,
                 FIELD_DP32(0, SHA_CLEAR_IRQ, CLEAR_INTERRUPT, 1));
    g_assert_cmphex(qtest_readl(qts, SHA_BASE + A_SHA_IRQ_ENA), ==, 1);

    qtest_quit(qts);
}

static void test_rng_modeled_surface(void)
{
    QTestState *qts = qts_start();
    uint32_t a = qtest_readl(qts, ESP32S3_RNG_BASE);
    uint32_t b = qtest_readl(qts, ESP32S3_RNG_BASE);

    g_assert_cmpuint(a, !=, 0);
    g_assert_cmpuint(b, !=, 0);
    g_assert_cmphex(qtest_readl(qts, ESP32S3_RNG_BASE + 4), ==, 0);

    qtest_quit(qts);
}

static void test_pms_modeled_surface(void)
{
    QTestState *qts = qts_start();
    const uint32_t test_data = 0x12345678;

    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x44), ==, 0);
    qtest_writel(qts, PMS_BASE + 0x44, test_data);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x44), ==, test_data);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x200), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0xffc), ==, 0x20260400);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/esp32s3/ana/pll-done", test_ana_pll_done);
    qtest_add_func("/esp32s3/gpio/iomux", test_gpio_and_iomux_state);
    qtest_add_func("/esp32s3/gpspi/completion-irq", test_gpspi_completion_irq);
    qtest_add_func("/esp32s3/gpio/remap-state", test_gpio_signal_remap_state);
    qtest_add_func("/esp32s3/gpio/remap-non-default", test_gpio_signal_non_default_routing);
    qtest_add_func("/esp32s3/spi1/usr-rx-overwrite", test_spi1_usr_rx_overwrites_all_bytes);
#ifndef _WIN32
    qtest_add_func("/esp32s3/jtag/rx-tx", test_usb_serial_jtag_rx_tx);
#endif
    qtest_add_func("/esp32s3/i2c/done-ack", test_i2c_done_and_ack_semantics);
    qtest_add_func("/esp32s3/i2c/unsupported-modes", test_i2c_unsupported_modes);
    qtest_add_func("/esp32s3/uart/clock-timing", test_uart_clock_dependent_timing);
    qtest_add_func("/esp32s3/sha/irq", test_sha_irq);
    qtest_add_func("/esp32s3/pms/modeled-surface", test_pms_modeled_surface);
    qtest_add_func("/esp32s3/rng/modeled-surface", test_rng_modeled_surface);

    return g_test_run();
}
