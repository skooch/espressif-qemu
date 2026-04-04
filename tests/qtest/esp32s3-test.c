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
#include "hw/char/esp32s3_uart.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"
#include "hw/i2c/esp32_i2c.h"
#include "hw/misc/esp32c3_jtag.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "hw/dma/esp32s3_gdma.h"
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
#define GDMA_BASE               DR_REG_GDMA_BASE
#define GPIO_BASE               DR_REG_GPIO_BASE
#define IOMUX_BASE              DR_REG_IO_MUX_BASE
#define JTAG_BASE               DR_REG_USB_SERIAL_JTAG_BASE
#define I2C0_BASE               DR_REG_I2C_EXT_BASE
#define UART0_BASE              DR_REG_UART_BASE
#define SHA_BASE                DR_REG_SHA_BASE
#define SYSTEM_BASE             DR_REG_SYSTEM_BASE
#define RTC_CNTL_BASE           DR_REG_RTCCNTL_BASE
#define PMS_BASE                DR_REG_SENSITIVE_BASE

#define GPSPI_CMD_UPDATE_BIT    BIT(23)
#define GPSPI_CMD_USR_BIT       BIT(24)
#define GPSPI_INT_TRANS_DONE    BIT(12)
#define USB_SERIAL_JTAG_EP1_DATA_FREE        BIT(1)
#define USB_SERIAL_JTAG_EP1_DATA_AVAIL       BIT(2)
#define USB_SERIAL_JTAG_EP1_WR_DONE         BIT(0)
#define GDMA_SPI2_CHAN          0
#define GDMA_OUT_DESC_SIZE      12
#define GDMA_TX_DATA_BASE       (ESP_GDMA_RAM_ADDR + 0x1000)
#define GDMA_RX_DATA_BASE       (ESP_GDMA_RAM_ADDR + 0x1100)
#define GDMA_TX_DESC_BASE       (ESP_GDMA_RAM_ADDR + 0x1200)
#define GDMA_RX_DESC_BASE       (ESP_GDMA_RAM_ADDR + 0x1300)
#define SHA_DMA_BUF_BASE        (ESP_GDMA_RAM_ADDR + 0x1400)
#define SHA_DMA_DESC_BASE       (ESP_GDMA_RAM_ADDR + 0x1500)

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

static uint32_t spi1_bytes_from_bits(uint32_t bit_len)
{
    return (bit_len + 1) / 8;
}

typedef struct TestGdmaLinkedList {
    union {
        struct {
            uint32_t size: 12;
            uint32_t length: 12;
            uint32_t rsvd_24: 4;
            uint32_t err_eof: 1;
            uint32_t rsvd_29: 1;
            uint32_t suc_eof: 1;
            uint32_t owner: 1;
        } bits;
        uint32_t val;
    } config;
    uint32_t buf_addr;
    uint32_t next_addr;
} TestGdmaLinkedList;

static void test_spi1_usr_length_matrix(void)
{
    QTestState *qts = qts_start();
    const struct {
        uint32_t mosi_bits;
        uint32_t miso_bits;
    } cases[] = {
        {7, 0},
        {0, 7},
        {15, 15},
        {31, 0},
        {0, 31},
        {31, 15},
    };

    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_USER,
                 R_SPI_MEM_USER_USR_COMMAND_MASK |
                 R_SPI_MEM_USER_USR_MOSI_MASK |
                 R_SPI_MEM_USER_USR_MISO_MASK);

    for (int i = 0; i < G_N_ELEMENTS(cases); i++) {
        const uint32_t mosi_bits = cases[i].mosi_bits;
        const uint32_t miso_bits = cases[i].miso_bits;
        const uint32_t expected_rx = spi1_bytes_from_bits(miso_bits);
        const uint32_t sentinel = 0xA5A5A5A5;

        qtest_writel(qts, SPI1_BASE + A_SPI_MEM_W0, sentinel);

        qtest_writel(qts, SPI1_BASE + A_SPI_MEM_MOSI_DLEN,
                     FIELD_DP32(0, SPI_MEM_MOSI_DLEN, USR_MOSI_DBITLEN,
                                mosi_bits));
        qtest_writel(qts, SPI1_BASE + A_SPI_MEM_MISO_DLEN,
                     FIELD_DP32(0, SPI_MEM_MISO_DLEN, USR_MISO_DBITLEN,
                                miso_bits));
        qtest_writel(qts, SPI1_BASE + A_SPI_MEM_CMD,
                     R_SPI_MEM_CMD_USR_MASK);

        if (expected_rx == 0) {
            g_assert_cmphex(qtest_readl(qts, SPI1_BASE + A_SPI_MEM_W0), ==, sentinel);
        } else {
            g_assert_cmphex(qtest_readl(qts, SPI1_BASE + A_SPI_MEM_W0), !=, sentinel);
        }
    }

    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_MOSI_DLEN, 0);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_MISO_DLEN, 0);

    qtest_quit(qts);
}

static void test_spi1_cmd_length_edges(void)
{
    QTestState *qts = qts_start();
    uint8_t w0_data[8] = {0};

    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_W0, 0xA5A5A5A5);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_W1, 0xA5A5A5A5);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_CMD, R_SPI_MEM_CMD_FLASH_RDID_MASK);
    qtest_memread(qts, SPI1_BASE + A_SPI_MEM_W0, w0_data, 3);
    g_assert_cmpuint(w0_data[0], !=, 0xA5);
    g_assert_cmpuint(w0_data[1], !=, 0xA5);
    g_assert_cmpuint(w0_data[2], !=, 0xA5);

    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_RD_STATUS, 0xA5A5A5A5);
    qtest_writel(qts, SPI1_BASE + A_SPI_MEM_CMD, R_SPI_MEM_CMD_FLASH_RDSR_MASK);
    g_assert_cmpuint(qtest_readl(qts, SPI1_BASE + A_SPI_MEM_RD_STATUS) & 0xff,
                     !=, 0xA5);
    g_assert_cmphex(qtest_readl(qts, SPI1_BASE + A_SPI_MEM_RD_STATUS) & 0xffffff00,
                    ==, 0xA5A5A500);

    qtest_quit(qts);
}

static void write_gdma_descriptor(QTestState *qts, uint32_t addr,
                                 uint32_t size, uint32_t length,
                                 bool suc_eof, bool owner,
                                 uint32_t buffer_addr, uint32_t next_addr)
{
    TestGdmaLinkedList desc = { 0 };

    desc.config.bits.size = size;
    desc.config.bits.length = length;
    desc.config.bits.suc_eof = suc_eof;
    desc.config.bits.owner = owner;
    desc.buf_addr = buffer_addr;
    desc.next_addr = next_addr;

    qtest_memwrite(qts, addr, &desc, sizeof(desc));
}

static uint32_t sha_dma_buf_addr(uint32_t block_len)
{
    return SHA_DMA_BUF_BASE;
}

static void test_gpspi_dma_txrx_handoff(void)
{
    QTestState *qts = qts_start();
    const uint32_t tx_len = 4;
    const uint32_t rx_len = 4;
    const uint32_t tx_buf = GDMA_TX_DATA_BASE;
    const uint32_t rx_buf = GDMA_RX_DATA_BASE;
    const uint32_t tx_desc = GDMA_TX_DESC_BASE;
    const uint32_t rx_desc = GDMA_RX_DESC_BASE;
    const uint32_t in_dir_base = GDMA_SPI2_CHAN * DMA_CHAN_REGS_SIZE
                               + ESP_GDMA_IN_IDX * DMA_DIR_REGS_SIZE;
    const uint32_t out_dir_base = GDMA_SPI2_CHAN * DMA_CHAN_REGS_SIZE
                               + ESP_GDMA_OUT_IDX * DMA_DIR_REGS_SIZE;
    const uint8_t tx_payload[4] = { 0x11, 0x22, 0x33, 0x44 };
    const uint8_t zero[4] = { 0 };

    qtest_memwrite(qts, tx_buf, tx_payload, sizeof(tx_payload));
    qtest_memwrite(qts, rx_buf, zero, sizeof(zero));

    write_gdma_descriptor(qts, tx_desc, tx_len, tx_len, true, false,
                          tx_buf, 0);
    write_gdma_descriptor(qts, rx_desc, rx_len, 0, false, false,
                          rx_buf, 0);

    qtest_writel(qts, GDMA_BASE + in_dir_base + A_DMA_PERI_SEL,
                 FIELD_DP32(0, GDMA_PERI_SEL, PERI_SEL, GDMA_SPI2));
    qtest_writel(qts, GDMA_BASE + out_dir_base + A_DMA_PERI_SEL,
                 FIELD_DP32(0, GDMA_PERI_SEL, PERI_SEL, GDMA_SPI2));

    qtest_writel(qts, GDMA_BASE + in_dir_base + A_DMA_LINK,
                 R_GDMA_IN_LINK_START_MASK | (rx_desc & R_GDMA_IN_LINK_ADDR_MASK));
    qtest_writel(qts, GDMA_BASE + out_dir_base + A_DMA_LINK,
                 R_GDMA_OUT_LINK_START_MASK | (tx_desc & R_GDMA_OUT_LINK_ADDR_MASK));

    qtest_writel(qts, IOMUX_BASE + ESP32S3_IOMUX_GPIO_REG(48),
                 ESP32S3_GPIO_IOMUX_FUNC_GPIO << ESP32S3_IOMUX_MCU_SEL_SHIFT);
    qtest_writel(qts, GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_REG(48),
                 ESP32S3_GPIO_SIG_SD_CS);
    qtest_writel(qts, GPIO_BASE + GPIO_ENABLE1_W1TS_REG, BIT(16));
    qtest_writel(qts, GPIO_BASE + GPIO_OUT1_W1TC_REG, BIT(16));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPIO_IN1_REG) & BIT(16),
                    ==, 0);

    qtest_writel(qts, SPI2_BASE + 0x10,
                 (1u << 27) | (1u << 28)); /* USR_MOSI/MISO */
    qtest_writel(qts, SPI2_BASE + 0x1C, 31); /* 4 bytes */
    qtest_writel(qts, SPI2_BASE + 0x34, GPSPI_INT_TRANS_DONE);
    qtest_writel(qts, SPI2_BASE + 0x38, GPSPI_INT_TRANS_DONE);
    qtest_writel(qts, SPI2_BASE + 0x00, GPSPI_CMD_USR_BIT);

    qtest_clock_step(qts, 600);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x3c) & GPSPI_INT_TRANS_DONE,
                    ==, 0);

    qtest_clock_step(qts, 1600);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x00) & GPSPI_CMD_USR_BIT, ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPI2_BASE + 0x3c) & GPSPI_INT_TRANS_DONE,
                    ==, GPSPI_INT_TRANS_DONE);

    g_assert_cmphex(qtest_readl(qts, GDMA_BASE + in_dir_base + A_DMA_INT_RAW) &
                    (R_GDMA_INTERRUPT_IN_DONE_MASK | R_GDMA_INTERRUPT_IN_SUC_EOF_MASK),
                    ==, R_GDMA_INTERRUPT_IN_DONE_MASK | R_GDMA_INTERRUPT_IN_SUC_EOF_MASK);

    {
        uint8_t rx_after[4];

        qtest_memread(qts, rx_buf, rx_after, sizeof(rx_after));
        for (size_t i = 0; i < sizeof(rx_after); i++) {
            g_assert_cmphex(rx_after[i], ==, 0xff);
        }
    }

    qtest_writel(qts, SPI2_BASE + 0x38, GPSPI_INT_TRANS_DONE);

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
    g_assert_cmphex(qtest_readl(qts, JTAG_BASE + USB_SERIAL_JTAG_EP1_CONF_REG) &
                    USB_SERIAL_JTAG_EP1_DATA_AVAIL,
                    ==, USB_SERIAL_JTAG_EP1_DATA_AVAIL);
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

static uint32_t i2c_cmd_s3_read(size_t bytes, bool ack_check)
{
    uint32_t v = FIELD_DP32(0, I2C_CMD, BYTE_NUM, bytes);
    v = FIELD_DP32(v, I2C_CMD, OPCODE, I2C_OPCODE_READ);
    v = FIELD_DP32(v, I2C_CMD, ACK_CHECK_EN, ack_check ? 1 : 0);
    return v;
}

static uint32_t i2c_cmd_s3_stop(void)
{
    return FIELD_DP32(0, I2C_CMD, OPCODE, 2);
}

static uint32_t i2c_cmd_s3_end(void)
{
    return FIELD_DP32(0, I2C_CMD, OPCODE, I2C_OPCODE_END);
}

static void i2c_start_transaction(QTestState *qts)
{
    uint32_t ctr = FIELD_DP32(0, I2C_CTR, MS_MODE, 1);
    ctr = FIELD_DP32(ctr, I2C_CTR, TRANS_START, 1);
    qtest_writel(qts, I2C0_BASE + A_I2C_CTR, ctr);
    qtest_clock_step(qts, 3000);
}

static void test_i2c_read_path_and_deferred_complete(void)
{
    QTestState *qts = qts_start();
    const uint32_t expected_rx_len = 1;
    uint32_t ctr;

    qtest_writel(qts, I2C0_BASE + A_I2C_FIFO_DATA, 0x68); /* 0x34 write */
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD,
                 i2c_cmd_write(1, true));
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD + 4,
                 i2c_cmd_s3_read(expected_rx_len, false));
    qtest_writel(qts, I2C0_BASE + A_I2C_CMD + 8, i2c_cmd_s3_stop());
    qtest_writel(qts, I2C0_BASE + A_I2C_INT_ENA,
                 R_I2C_INT_ENA_TRANS_COMPLETE_MASK |
                 R_I2C_INT_ENA_END_DETECT_MASK);

    ctr = FIELD_DP32(0, I2C_CTR, MS_MODE, 1);
    ctr = FIELD_DP32(ctr, I2C_CTR, TRANS_START, 1);
    qtest_writel(qts, I2C0_BASE + A_I2C_CTR, ctr);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    (R_I2C_INT_RAW_TRANS_COMPLETE_MASK |
                     R_I2C_INT_RAW_END_DETECT_MASK), ==, 0);

    qtest_clock_step(qts, 1500);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_TRANS_COMPLETE_MASK,
                    ==, R_I2C_INT_RAW_TRANS_COMPLETE_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_END_DETECT_MASK, ==, 0);
    g_assert_cmpuint(FIELD_EX32(qtest_readl(qts, I2C0_BASE + A_I2C_STATUS),
                               I2C_STATUS, RXFIFO_CNT), ==, expected_rx_len);

    qtest_writel(qts, I2C0_BASE + A_I2C_CMD, i2c_cmd_s3_end());
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_END_DETECT_MASK, ==, 0);

    ctr = FIELD_DP32(0, I2C_CTR, MS_MODE, 1);
    ctr = FIELD_DP32(ctr, I2C_CTR, TRANS_START, 1);
    qtest_writel(qts, I2C0_BASE + A_I2C_CTR, ctr);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_END_DETECT_MASK, ==, 0);
    qtest_clock_step(qts, 1500);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW) &
                    R_I2C_INT_RAW_END_DETECT_MASK,
                    ==, R_I2C_INT_RAW_END_DETECT_MASK);

    qtest_writel(qts, I2C0_BASE + A_I2C_INT_CLR,
                 R_I2C_INT_CLR_TRANS_COMPLETE_MASK |
                 R_I2C_INT_CLR_END_DETECT_MASK |
                 R_I2C_INT_CLR_ACK_ERR_MASK);
    g_assert_cmphex(qtest_readl(qts, I2C0_BASE + A_I2C_INT_RAW), ==, 0);

    qtest_quit(qts);
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

static void test_rtc_clk_update_propagates_to_system_and_uart(void)
{
    QTestState *qts = qts_start();
    uint32_t default_pulse = qtest_readl(qts, UART0_BASE + A_UART_LOWPULSE);
    uint32_t rtc_clk_conf = 0;
    uint32_t sysclk_conf;

    rtc_clk_conf = FIELD_DP32(rtc_clk_conf, RTC_CNTL_CLK_CONF, ANA_CLK_RTC_SEL,
                              ESP32_SLOW_CLK_RC);
    rtc_clk_conf = FIELD_DP32(rtc_clk_conf, RTC_CNTL_CLK_CONF, FAST_CLK_RTC_SEL,
                              ESP32_FAST_CLK_8M);
    rtc_clk_conf = FIELD_DP32(rtc_clk_conf, RTC_CNTL_CLK_CONF, SOC_CLK_SEL,
                              ESP32_SOC_CLK_XTAL);
    qtest_writel(qts, DR_REG_RTCCNTL_BASE + A_RTC_CNTL_CLK_CONF, rtc_clk_conf);

    sysclk_conf = qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_SYSCLK_CONF);
    g_assert_cmpuint(FIELD_EX32(sysclk_conf, SYSTEM_SYSCLK_CONF, SOC_CLK_SEL),
                     ==, ESP32S3_CLK_SEL_XTAL);
    g_assert_cmpuint(qtest_readl(qts, UART0_BASE + A_UART_LOWPULSE), <, default_pulse);

    qtest_writel(qts, UART0_BASE + A_UART_AUTOBAUD,
                 FIELD_DP32(0, UART_AUTOBAUD, EN, 1));
    g_assert_cmpuint(qtest_readl(qts, UART0_BASE + A_UART_RXD_CNT), ==, 400);

    qtest_quit(qts);
}

static void test_rtc_timer_wakeup_transition(void)
{
    QTestState *qts = qts_start();
    const uint32_t alarm_ticks = 30;
    uint32_t timer1 = 0;
    uint32_t wakeup_state = 0;
    uint32_t state0 = 0;

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER0, alarm_ticks);
    timer1 = FIELD_DP32(timer1, RTC_CNTL_SLP_TIMER1, MAIN_TIMER_ALARM_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER1, timer1);

    wakeup_state = FIELD_DP32(wakeup_state, RTC_CNTL_WAKEUP_STATE,
                              TIMER_WAKEUP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);

    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLEEP_EN, 1);
    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLP_WAKEUP, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, state0);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);

    qtest_clock_step(qts, 150000);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);

    qtest_clock_step(qts, 60000);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_WAKEUP_CAUSE),
                    ==, R_RTC_CNTL_SLP_WAKEUP_CAUSE_TIMER_MASK);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR,
                 R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);

    qtest_quit(qts);
}

static void test_rtc_reset_transitions(void)
{
    QTestState *qts = qts_start();
    uint32_t options0 = 0;
    uint32_t reset_state;

    options0 = FIELD_DP32(options0, RTC_CNTL_OPTIONS0, SW_PROCPU_RESET, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);
    qtest_qmp_eventwait(qts, "RESET");

    reset_state = qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_RESET_STATE);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_PROCPU), ==, ESP32_SW_CPU_RESET);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_APPCPU), ==, ESP32_POWERON_RESET);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0) &
                    R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK, ==, 0);

    options0 = FIELD_DP32(0, RTC_CNTL_OPTIONS0, SW_SYS_RESET, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);
    qtest_qmp_eventwait(qts, "RESET");

    reset_state = qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_RESET_STATE);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_PROCPU), ==, ESP32_SW_SYS_RESET);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_APPCPU), ==, ESP32_SW_SYS_RESET);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0) &
                    R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK, ==, 0);

    qtest_quit(qts);
}

static void test_rtc_cpu_stall_transition(void)
{
    QTestState *qts = qts_start();
    uint32_t sw_cpu_stall = 0;
    uint32_t options0 = 0;

    qtest_irq_intercept_out_named(qts, "/machine/soc/rtc_cntl",
                                  ESP32S3_RTC_CPU_STALL_GPIO);

    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    sw_cpu_stall = FIELD_DP32(sw_cpu_stall, RTC_CNTL_SW_CPU_STALL,
                              APPCPU_C1, 0x21);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SW_CPU_STALL, sw_cpu_stall);
    g_assert_false(qtest_get_irq(qts, 1));

    options0 = FIELD_DP32(options0, RTC_CNTL_OPTIONS0, SW_STALL_APPCPU_C0, 2);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, 0);
    g_assert_false(qtest_get_irq(qts, 1));

    sw_cpu_stall = FIELD_DP32(0, RTC_CNTL_SW_CPU_STALL, PROCPU_C1, 0x21);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SW_CPU_STALL, sw_cpu_stall);

    options0 = FIELD_DP32(0, RTC_CNTL_OPTIONS0, SW_STALL_PROCPU_C0, 2);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static uint64_t wait_for_uart_timeout_ns(QTestState *qts, uint64_t step_ns, uint64_t max_ns)
{
    uint64_t elapsed = 0;

    while (elapsed <= max_ns) {
        if (qtest_readl(qts, UART0_BASE + A_UART_INT_RAW) &
            R_UART_INT_RAW_RXFIFO_TOUT_MASK) {
            return elapsed;
        }
        qtest_clock_step(qts, step_ns);
        elapsed += step_ns;
    }

    return max_ns + 1;
}

static void test_uart_rx_timeout_multi_config(void)
{
    g_autofree gchar *sock_dir = g_dir_make_tmp("esp32s3-uart-XXXXXX", NULL);
    g_autofree gchar *sock_path = g_build_filename(sock_dir, "uart.sock", NULL);
    QTestState *qts;
    int sock_fd;
    uint64_t short_timeout;
    uint64_t long_threshold_timeout;
    uint64_t fast_baud_timeout;
    uint64_t slow_baud_timeout;
    char rx_byte = 'Z';

    qts = qtest_initf("-M esp32s3 "
                      "-chardev socket,id=uart0,path=%s,server=on,wait=off "
                      "-serial chardev:uart0", sock_path);
    sock_fd = connect_unix_socket(sock_path);

    qtest_writel(qts, UART0_BASE + A_UART_INT_ENA,
                 R_UART_INT_ENA_RXFIFO_TOUT_MASK);

    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_MEM_CONF,
                 FIELD_DP32(0, ESP32S3_UART_MEM_CONF, RX_TOUT_THRHD, 1));
    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_CONF1,
                 FIELD_DP32(0, ESP32S3_UART_CONF1, RX_TOUT_EN, 1));
    qtest_writel(qts, UART0_BASE + A_UART_INT_CLR, R_UART_INT_CLR_RXFIFO_TOUT_MASK);
    g_assert_cmpint(write(sock_fd, &rx_byte, 1), ==, 1);
    qtest_clock_step(qts, 1000);
    short_timeout = wait_for_uart_timeout_ns(qts, 100, 5000000);
    g_assert_cmpuint(short_timeout, <=, 5000000);

    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_MEM_CONF,
                 FIELD_DP32(0, ESP32S3_UART_MEM_CONF, RX_TOUT_THRHD, 4));
    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_CONF1,
                 FIELD_DP32(0, ESP32S3_UART_CONF1, RX_TOUT_EN, 1));
    qtest_writel(qts, UART0_BASE + A_UART_INT_CLR, R_UART_INT_CLR_RXFIFO_TOUT_MASK);
    g_assert_cmpint(write(sock_fd, &rx_byte, 1), ==, 1);
    qtest_clock_step(qts, 1000);
    long_threshold_timeout = wait_for_uart_timeout_ns(qts, 100, 5000000);
    g_assert_cmpuint(long_threshold_timeout, >, short_timeout);

    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_MEM_CONF,
                 FIELD_DP32(0, ESP32S3_UART_MEM_CONF, RX_TOUT_THRHD, 1));
    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_CONF1,
                 FIELD_DP32(0, ESP32S3_UART_CONF1, RX_TOUT_EN, 1));
    qtest_writel(qts, UART0_BASE + A_UART_INT_CLR, R_UART_INT_CLR_RXFIFO_TOUT_MASK);
    qtest_writel(qts, UART0_BASE + A_UART_CLKDIV,
                 FIELD_DP32(0, UART_CLKDIV, CLKDIV, 0x400));
    g_assert_cmpint(write(sock_fd, &rx_byte, 1), ==, 1);
    qtest_clock_step(qts, 1000);
    fast_baud_timeout = wait_for_uart_timeout_ns(qts, 100, 5000000);

    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_MEM_CONF,
                 FIELD_DP32(0, ESP32S3_UART_MEM_CONF, RX_TOUT_THRHD, 1));
    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_CONF1,
                 FIELD_DP32(0, ESP32S3_UART_CONF1, RX_TOUT_EN, 1));
    qtest_writel(qts, UART0_BASE + A_UART_INT_CLR, R_UART_INT_CLR_RXFIFO_TOUT_MASK);
    qtest_writel(qts, UART0_BASE + A_UART_CLKDIV,
                 FIELD_DP32(0, UART_CLKDIV, CLKDIV, 0x800));
    g_assert_cmpint(write(sock_fd, &rx_byte, 1), ==, 1);
    qtest_clock_step(qts, 1000);
    slow_baud_timeout = wait_for_uart_timeout_ns(qts, 100, 5000000);

    g_assert_cmpuint(slow_baud_timeout, >, fast_baud_timeout);

    qtest_writel(qts, UART0_BASE + A_ESP32S3_UART_CONF1, 0);
    qtest_writel(qts, UART0_BASE + A_UART_INT_CLR, R_UART_INT_CLR_RXFIFO_TOUT_MASK);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readl(qts, UART0_BASE + A_UART_INT_RAW) &
                    R_UART_INT_RAW_RXFIFO_TOUT_MASK, ==, 0);

    close(sock_fd);
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

static void test_sha_dma_start_and_continue_irq_paths(void)
{
    QTestState *qts = qts_start();
    const uint32_t block_len = 64;
    const uint8_t block0[64] = { 0 };
    const uint8_t block1[64] = { 0xbb };
    const uint32_t sha_desc = SHA_DMA_DESC_BASE;
    uint32_t hash_start;
    uint32_t hash_continue;

    qtest_memwrite(qts, sha_dma_buf_addr(block_len), block0, block_len);
    write_gdma_descriptor(qts, sha_desc, block_len, block_len, true, false,
                          sha_dma_buf_addr(block_len), 0);
    g_assert_cmpuint(qtest_readl(qts, sha_desc + 0) & 0xfff, ==, block_len);
    g_assert_cmpuint((qtest_readl(qts, sha_desc + 0) >> 12) & 0xfff, ==, block_len);
    g_assert_cmphex(qtest_readl(qts, sha_desc + 4), ==,
                    sha_dma_buf_addr(block_len));
    qtest_writel(qts, GDMA_BASE + GDMA_SPI2_CHAN * DMA_CHAN_REGS_SIZE +
                 ESP_GDMA_OUT_IDX * DMA_DIR_REGS_SIZE + A_DMA_PERI_SEL,
                 FIELD_DP32(0, GDMA_PERI_SEL, PERI_SEL, GDMA_SHA));
    qtest_writel(qts, GDMA_BASE + GDMA_SPI2_CHAN * DMA_CHAN_REGS_SIZE +
                 ESP_GDMA_OUT_IDX * DMA_DIR_REGS_SIZE + A_DMA_LINK,
                 R_GDMA_OUT_LINK_START_MASK | (sha_desc & R_GDMA_OUT_LINK_ADDR_MASK));
    g_assert_cmphex(qtest_readl(qts, GDMA_BASE + GDMA_SPI2_CHAN * DMA_CHAN_REGS_SIZE +
                    ESP_GDMA_OUT_IDX * DMA_DIR_REGS_SIZE + A_DMA_PERI_SEL),
                    ==, FIELD_DP32(0, GDMA_PERI_SEL, PERI_SEL, GDMA_SHA));
    g_assert_cmphex(qtest_readl(qts, GDMA_BASE + GDMA_SPI2_CHAN * DMA_CHAN_REGS_SIZE +
                    ESP_GDMA_OUT_IDX * DMA_DIR_REGS_SIZE + A_DMA_LINK) &
                    R_GDMA_OUT_LINK_START_MASK,
                    ==, R_GDMA_OUT_LINK_START_MASK);

    qtest_writel(qts, SHA_BASE + A_SHA_MODE, ESP_SHA_256_MODE);
    qtest_writel(qts, SHA_BASE + A_SHA_DMA_BLOCK_NUM, 1);
    qtest_writel(qts, SHA_BASE + A_SHA_IRQ_ENA,
                 FIELD_DP32(0, SHA_IRQ_ENA, INTERRUPT_ENA, 1));
    qtest_writel(qts, SHA_BASE + A_SHA_DMA_START,
                 FIELD_DP32(0, SHA_DMA_START, DMA_START, 1));

    hash_start = qtest_readl(qts, SHA_BASE + A_SHA_H_MEM);
    g_assert_cmpuint(hash_start, !=, 0);

    qtest_writel(qts, SHA_BASE + A_SHA_CLEAR_IRQ,
                 FIELD_DP32(0, SHA_CLEAR_IRQ, CLEAR_INTERRUPT, 1));

    qtest_memwrite(qts, sha_dma_buf_addr(block_len), block1, block_len);
    write_gdma_descriptor(qts, sha_desc, block_len, block_len, true, false,
                          sha_dma_buf_addr(block_len), 0);
    qtest_writel(qts, SHA_BASE + A_SHA_DMA_CONTINUE,
                 FIELD_DP32(0, SHA_DMA_CONTINUE, DMA_CONTINUE, 1));

    hash_continue = qtest_readl(qts, SHA_BASE + A_SHA_H_MEM);
    g_assert_cmpuint(hash_continue, !=, hash_start);

    qtest_writel(qts, SHA_BASE + A_SHA_CLEAR_IRQ,
                 FIELD_DP32(0, SHA_CLEAR_IRQ, CLEAR_INTERRUPT, 1));

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
    qtest_add_func("/esp32s3/spi1/usr-length-matrix", test_spi1_usr_length_matrix);
    qtest_add_func("/esp32s3/spi1/cmd-length-edges", test_spi1_cmd_length_edges);
#ifndef _WIN32
    qtest_add_func("/esp32s3/jtag/rx-tx", test_usb_serial_jtag_rx_tx);
#endif
    qtest_add_func("/esp32s3/i2c/done-ack", test_i2c_done_and_ack_semantics);
    qtest_add_func("/esp32s3/i2c/read-path-deferred", test_i2c_read_path_and_deferred_complete);
    qtest_add_func("/esp32s3/i2c/unsupported-modes", test_i2c_unsupported_modes);
    qtest_add_func("/esp32s3/rtc/clk-update", test_rtc_clk_update_propagates_to_system_and_uart);
    qtest_add_func("/esp32s3/rtc/timer-wakeup", test_rtc_timer_wakeup_transition);
    qtest_add_func("/esp32s3/rtc/reset-transitions", test_rtc_reset_transitions);
    qtest_add_func("/esp32s3/rtc/cpu-stall", test_rtc_cpu_stall_transition);
    qtest_add_func("/esp32s3/uart/clock-timing", test_uart_clock_dependent_timing);
    qtest_add_func("/esp32s3/uart/rx-timeout-multi-config", test_uart_rx_timeout_multi_config);
    qtest_add_func("/esp32s3/sha/irq", test_sha_irq);
    qtest_add_func("/esp32s3/sha/dma-start-continue", test_sha_dma_start_and_continue_irq_paths);
    qtest_add_func("/esp32s3/gpspi/dma-tx-rx-handoff", test_gpspi_dma_txrx_handoff);
    qtest_add_func("/esp32s3/pms/modeled-surface", test_pms_modeled_surface);
    qtest_add_func("/esp32s3/rng/modeled-surface", test_rng_modeled_surface);

    return g_test_run();
}
