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
#include "hw/misc/esp32s3_cache.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "hw/dma/esp32s3_gdma.h"
#include "hw/net/mii.h"
#include "hw/misc/esp32s3_rng.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/misc/esp_sha.h"
#include "hw/nvram/esp32s3_efuse.h"
#include "hw/ssi/esp32s3_gpspi.h"
#include "hw/ssi/esp32s3_spi.h"
#include "hw/timer/esp_timg.h"
#include "hw/xtensa/esp32s3_clk.h"
#include "hw/xtensa/esp32s3_clk_defs.h"

#define ANA_BASE                0x6000e000
#define SPI2_BASE               DR_REG_SPI2_BASE
#define SPI0_BASE               DR_REG_SPI0_BASE
#define SPI1_BASE               DR_REG_SPI1_BASE
#define GDMA_BASE               DR_REG_GDMA_BASE
#define GPIO_BASE               DR_REG_GPIO_BASE
#define IOMUX_BASE              DR_REG_IO_MUX_BASE
#define JTAG_BASE               DR_REG_USB_SERIAL_JTAG_BASE
#define I2C0_BASE               DR_REG_I2C_EXT_BASE
#define EMAC_BASE               DR_REG_EMAC_BASE
#define UART0_BASE              DR_REG_UART_BASE
#define TIMG0_BASE              DR_REG_TIMERGROUP0_BASE
#define SHA_BASE                DR_REG_SHA_BASE
#define SYSTEM_BASE             DR_REG_SYSTEM_BASE
#define INTMATRIX_BASE          DR_REG_INTERRUPT_BASE
#define APB_CTRL_BASE           DR_REG_APB_CTRL_BASE
#define APB_SARADC_BASE         DR_REG_APB_SARADC_BASE
#define SENS_BASE               DR_REG_SENS_BASE
#define RTC_CNTL_BASE           DR_REG_RTCCNTL_BASE
#define EFUSE_BASE              DR_REG_EFUSE_BASE
#define PMS_BASE                DR_REG_SENSITIVE_BASE
#define ASSIST_DEBUG_BASE       DR_REG_ASSIST_DEBUG_BASE
#define LEDC_BASE               DR_REG_LEDC_BASE
#define GENERIC_MMIO_LEDC_REG   (LEDC_BASE + 0x0f0)
#define GENERIC_MMIO_LEDC_REG2  (GENERIC_MMIO_LEDC_REG + 4)
#define GENERIC_MMIO_FE2_REG    (DR_REG_FE2_BASE + 0x0f0)
#define GENERIC_MMIO_CORE_REG   (DR_REG_WCL_BASE + 0x0f0)
#define RTC_EXT_WAKEUP_CONF_REG (RTC_CNTL_BASE + 0x64)
#define RTC_EXT_WAKEUP1_REG     (RTC_CNTL_BASE + 0xe0)
#define RTC_RETENTION_CTRL_REG  (RTC_CNTL_BASE + 0x140)
#define RTC_WAKEUP_ENA_EXT1_BIT BIT(16)
#define CACHE_OP_DELAY_NS              1000
#define ESP32S3_CACHE_IA_SOURCE        56
#define ESP32S3_CACHE_CORE0_ACS_SOURCE 94
#define ESP32S3_EMAC_SOURCE            0
#define ESP32S3_GPIO_SOURCE            16
#define APB_CTRL_LEGACY_ECO3_DATE_REG  (APB_CTRL_BASE + 0x07c)
#define APB_CTRL_QEMU_ORIGIN_REG       (APB_CTRL_BASE + 0x3f8)
#define APB_CTRL_DATE_REG              (APB_CTRL_BASE + 0x3fc)
#define APB_CTRL_UNSUPPORTED_REG       (APB_CTRL_BASE + 0x080)
#define APB_CTRL_DATE_VALUE            0x02101150
#define APB_CTRL_LEGACY_ECO3_DATE      0x96042000
#define APB_CTRL_QEMU_ORIGIN           0x51454d55
#define APB_SARADC_CTRL_DEFAULT        0x407f8240
#define APB_SARADC_CTRL2_DEFAULT       0x0000a1fe
#define APB_SARADC_ARB_CTRL_DEFAULT    0x00000900
#define APB_SARADC_FILTER0_DEFAULT     0x006b4000
#define APB_SARADC_CLKM_CONF_DEFAULT   0x00000004
#define INTMATRIX_CPU_STRIDE           0x800
#define INTMATRIX_INTR_STATUS0         0x18c
#define INTMATRIX_MAP_REG(cpu, source) \
    (INTMATRIX_BASE + ((cpu) * INTMATRIX_CPU_STRIDE) + ((source) * 4))
#define INTMATRIX_STATUS_REG(cpu, word) \
    (INTMATRIX_BASE + ((cpu) * INTMATRIX_CPU_STRIDE) + \
     INTMATRIX_INTR_STATUS0 + ((word) * 4))
#define SYSTEM_CPU_PER_CONF_SUPPORTED_MASK \
    (R_SYSTEM_CPU_PER_CONF_CPU_WAITI_DELAY_NUM_MASK | \
     R_SYSTEM_CPU_PER_CONF_CPU_WAIT_MODE_FORCE_ON_MASK | \
     R_SYSTEM_CPU_PER_CONF_PLL_FREQ_SEL_MASK | \
     R_SYSTEM_CPU_PER_CONF_CPUPERIOD_SEL_MASK)
#define SYSTEM_SYSCLK_CONF_SUPPORTED_MASK \
    (R_SYSTEM_SYSCLK_CONF_CLK_DIV_EN_MASK | \
     R_SYSTEM_SYSCLK_CONF_CLK_XTAL_FREQ_MASK | \
     R_SYSTEM_SYSCLK_CONF_SOC_CLK_SEL_MASK | \
     R_SYSTEM_SYSCLK_CONF_PRE_DIV_CNT_MASK)
#define EFUSE_OP_DELAY_NS       100000
#define EFUSE_READ_DONE         BIT(0)
#define EFUSE_PGM_DONE          BIT(1)
#define EFUSE_WRITE_OPCODE      0x5a5a
#define EFUSE_READ_OPCODE       0x5aa5
#define PMS_DATE_VALUE          0x02101280

#define OPENETH_MODER_DEFAULT          0xa000
#define OPENETH_MODER_LOOPBCK          BIT(7)
#define OPENETH_MODER_PRO              BIT(5)
#define OPENETH_MODER_TXEN             BIT(1)
#define OPENETH_MODER_RXEN             BIT(0)
#define OPENETH_INT_SOURCE_RXB         BIT(2)
#define OPENETH_INT_SOURCE_TXB         BIT(0)
#define OPENETH_MIICOMMAND_RSTAT       BIT(1)
#define OPENETH_MIICOMMAND_SCANSTAT    BIT(0)
#define OPENETH_MIISTATUS_LINKFAIL     BIT(0)
#define OPENETH_DEFAULT_PHY            1
#define OPENETH_MIIADDRESS_RGAD_SHIFT  8
#define OPENETH_MIIADDRESS_FIAD_SHIFT  0
#define OPENETH_TXD_RD                 BIT(15)
#define OPENETH_TXD_IRQ                BIT(14)
#define OPENETH_RXD_E                  BIT(15)
#define OPENETH_RXD_IRQ                BIT(14)
#define OPENETH_RXD_M                  BIT(7)

#define OPENETH_INT_SOURCE_REG         (EMAC_BASE + 0x04)
#define OPENETH_INT_MASK_REG           (EMAC_BASE + 0x08)
#define OPENETH_TX_BD_NUM_REG          (EMAC_BASE + 0x20)
#define OPENETH_MIICOMMAND_REG         (EMAC_BASE + 0x2c)
#define OPENETH_MIIADDRESS_REG         (EMAC_BASE + 0x30)
#define OPENETH_MIIRX_DATA_REG         (EMAC_BASE + 0x38)
#define OPENETH_MIISTATUS_REG          (EMAC_BASE + 0x3c)
#define OPENETH_MAC_ADDR0_REG          (EMAC_BASE + 0x40)
#define OPENETH_MAC_ADDR1_REG          (EMAC_BASE + 0x44)
#define OPENETH_DESC_BASE              (EMAC_BASE + 0x400)
#define OPENETH_DESC_SIZE              8
#define OPENETH_RX_DESC_INDEX          0x40
#define OPENETH_TX_DESC0_ADDR          (OPENETH_DESC_BASE + 0 * OPENETH_DESC_SIZE)
#define OPENETH_RX_DESC0_ADDR          (OPENETH_DESC_BASE + OPENETH_RX_DESC_INDEX * OPENETH_DESC_SIZE)
#define OPENETH_TX_BUF_BASE            (ESP_GDMA_RAM_ADDR + 0x2000)
#define OPENETH_RX_BUF_BASE            (ESP_GDMA_RAM_ADDR + 0x2100)

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

G_STATIC_ASSERT(A_EXTMEM_DCACHE_SYNC_ADDR == A_EXTMEM_DCACHE_SYNC_CTRL + 4);
G_STATIC_ASSERT(A_EXTMEM_DCACHE_SYNC_SIZE == A_EXTMEM_DCACHE_SYNC_ADDR + 4);
G_STATIC_ASSERT(A_EXTMEM_ICACHE_SYNC_ADDR == A_EXTMEM_ICACHE_SYNC_CTRL + 4);
G_STATIC_ASSERT(A_EXTMEM_ICACHE_SYNC_SIZE == A_EXTMEM_ICACHE_SYNC_ADDR + 4);

static QTestState *qts_start(void)
{
    return qtest_init("-M esp32s3");
}

static QTestState *qts_start_smp1(void)
{
    return qtest_init("-M esp32s3 -smp 1");
}

static QTestState *qts_start_with_psram(void)
{
    return qtest_init("-M esp32s3 -m 2M");
}

static QTestState *qts_start_with_openeth(void)
{
    return qtest_init("-M esp32s3 -nic user,id=emac0,model=open_eth");
}

static QTestState *qts_start_without_openeth(void)
{
    return qtest_init("-M esp32s3 -nic none");
}

static void assert_openeth_instantiated(QTestState *qts, bool expected)
{
    g_autofree char *qtree = qtest_hmp(qts, "info qtree");
    bool present = g_strstr_len(qtree, -1, "open_eth") != NULL;

    g_assert_cmpint(present, ==, expected);
}

static void test_generic_mmio_compatibility_storage(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_LEDC_REG), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_LEDC_REG2), ==, 0);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_FE2_REG), ==, 0);

    qtest_writel(qts, GENERIC_MMIO_LEDC_REG, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_LEDC_REG), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_LEDC_REG2), ==, 0);

    qtest_writel(qts, GENERIC_MMIO_LEDC_REG2, 0xa5a55a5a);
    qtest_writel(qts, GENERIC_MMIO_FE2_REG, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_LEDC_REG), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_LEDC_REG2), ==, 0xa5a55a5a);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_FE2_REG), ==, 0x55667788);

    qtest_writel(qts, GENERIC_MMIO_CORE_REG, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, GENERIC_MMIO_CORE_REG), ==, 0);

    qtest_quit(qts);
}

static void test_spi0_mem_register_surface(void)
{
    QTestState *qts = qts_start();
    const uint32_t stored_regs[] = {
        A_SPI_MEM_CTRL,
        A_SPI_MEM_CTRL2,
        A_SPI_MEM_CLOCK,
        A_SPI_MEM_USER,
        A_SPI_MEM_USER1,
        A_SPI_MEM_USER2,
        A_SPI_MEM_MOSI_DLEN,
        A_SPI_MEM_MISO_DLEN,
        A_SPI_MEM_MISC,
        A_SPI_MEM_CACHE_FCTRL,
        A_SPI_MEM_CACHE_SCTRL,
        A_SPI_MEM_SRAM_DRD_CMD,
        A_SPI_MEM_SRAM_DWR_CMD,
        A_SPI_MEM_SRAM_CLK,
        A_SPI_MEM_SPI_SMEM_AC,
        A_SPI_MEM_DDR_CTRL,
        A_SPI_MEM_CLOCK_GATE,
        A_SPI_MEM_CORE_CLK_SEL,
    };

    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + A_SPI_MEM_FSM), ==, 0);

    for (int i = 0; i < G_N_ELEMENTS(stored_regs); i++) {
        uint32_t value = 0x5a000000u | (stored_regs[i] << 8) | i;

        qtest_writel(qts, SPI0_BASE + stored_regs[i], value);
        g_assert_cmphex(qtest_readl(qts, SPI0_BASE + stored_regs[i]), ==,
                        value);
    }

    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + A_SPI_MEM_FSM), ==, 0);

    qtest_quit(qts);
}

static void test_assist_debug_register_surface(void)
{
    QTestState *qts = qts_start();
    const uint32_t pdebugenable = ASSIST_DEBUG_BASE + 0x48;
    const uint32_t recording = ASSIST_DEBUG_BASE + 0x4c;
    const uint32_t pdebugpc = ASSIST_DEBUG_BASE + 0x5c;

    g_assert_cmphex(qtest_readl(qts, pdebugenable), ==, 0);
    g_assert_cmphex(qtest_readl(qts, recording), ==, 0);
    g_assert_cmphex(qtest_readl(qts, pdebugpc), ==, 0);

    qtest_writel(qts, pdebugenable, 0x1);
    qtest_writel(qts, recording, 0x2);
    qtest_writel(qts, pdebugpc, 0xffffffff);

    g_assert_cmphex(qtest_readl(qts, pdebugenable), ==, 0x1);
    g_assert_cmphex(qtest_readl(qts, recording), ==, 0x2);
    g_assert_cmphex(qtest_readl(qts, pdebugpc), ==, 0);

    qtest_quit(qts);
}

static void set_gpio_input_level(QTestState *qts, int gpio_num, bool level)
{
    qtest_set_irq_in(qts, "/machine/soc/gpio", ESP32S3_GPIO_INPUT_GPIO,
                     gpio_num, level);
}

#ifndef _WIN32
static QTestState *qts_start_with_flash(const char *flash_path)
{
    return qtest_initf("-M esp32s3 -drive file=%s,format=raw,if=mtd",
                       flash_path);
}

static gchar *create_flash_image_with_patterns(void)
{
    g_autofree gchar *tmp_path = NULL;
    int fd;
    uint32_t word;

    fd = g_file_open_tmp("esp32s3-flash-XXXXXX", &tmp_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 2 * 1024 * 1024), ==, 0);

    word = cpu_to_le32(0x11111111);
    g_assert_cmpint(pwrite(fd, &word, sizeof(word), 0), ==, sizeof(word));
    word = cpu_to_le32(0x22222222);
    g_assert_cmpint(pwrite(fd, &word, sizeof(word), ESP32S3_PAGE_SIZE),
                    ==, sizeof(word));

    close(fd);
    return g_steal_pointer(&tmp_path);
}

static void test_cache_flash_mmu_mapping_and_ctrl1_state(void)
{
    g_autofree gchar *flash_path = create_flash_image_with_patterns();
    QTestState *qts = qts_start_with_flash(flash_path);
    const uint32_t mmu_entry0 = DR_REG_EXTMEM_BASE + ESP32S3_MMU_TABLE_OFFSET;
    const uint32_t dcache_ctrl1_bits =
        R_EXTMEM_DCACHE_CTRL1_SHUT_DBUS_MASK |
        R_EXTMEM_DCACHE_CTRL1_SHUT_IBUS_MASK;
    const uint32_t icache_ctrl1_bits =
        R_EXTMEM_ICACHE_CTRL1_SHUT_DBUS_MASK |
        R_EXTMEM_ICACHE_CTRL1_SHUT_IBUS_MASK;

    g_assert_cmphex(qtest_readl(qts, mmu_entry0), ==, BIT(14));

    qtest_writel(qts, mmu_entry0, 0);
    g_assert_cmphex(qtest_readl(qts, 0x3c000000), ==, 0x11111111);

    qtest_writel(qts, mmu_entry0, 1);
    g_assert_cmphex(qtest_readl(qts, 0x3c000000), ==, 0x22222222);

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_CTRL1,
                 dcache_ctrl1_bits);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_CTRL1) &
                    dcache_ctrl1_bits,
                    ==, dcache_ctrl1_bits);

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_ICACHE_CTRL1,
                 icache_ctrl1_bits);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_ICACHE_CTRL1) &
                    icache_ctrl1_bits,
                    ==, icache_ctrl1_bits);

    qtest_quit(qts);
    unlink(flash_path);
}

static void test_cache_psram_mmu_mapping(void)
{
    QTestState *qts = qts_start_with_psram();
    const uint32_t mmu_entry0 = DR_REG_EXTMEM_BASE + ESP32S3_MMU_TABLE_OFFSET;
    const uint32_t psram_page0 = BIT(15);

    g_assert_cmphex(qtest_readl(qts, mmu_entry0), ==, BIT(14));

    qtest_writel(qts, mmu_entry0, psram_page0);
    qtest_writel(qts, ESP32S3_DCACHE_BASE, 0x13572468);
    g_assert_cmphex(qtest_readl(qts, ESP32S3_DCACHE_BASE), ==, 0x13572468);

    qtest_quit(qts);
}

static void assert_cache_deferred_completion(QTestState *qts, uint32_t reg,
                                             uint32_t trigger_bits,
                                             uint32_t ena_mask,
                                             uint32_t done_mask,
                                             uint32_t idle_mask)
{
    uint32_t reg_value;

    qtest_writel(qts, DR_REG_EXTMEM_BASE + reg, trigger_bits);
    reg_value = qtest_readl(qts, DR_REG_EXTMEM_BASE + reg);
    g_assert_cmphex(reg_value & ena_mask, ==, trigger_bits & ena_mask);
    g_assert_cmphex(reg_value & done_mask, ==, 0);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_STATE) &
                    idle_mask,
                    ==, 0);

    qtest_clock_step(qts, CACHE_OP_DELAY_NS - 1);
    reg_value = qtest_readl(qts, DR_REG_EXTMEM_BASE + reg);
    g_assert_cmphex(reg_value & ena_mask, ==, trigger_bits & ena_mask);
    g_assert_cmphex(reg_value & done_mask, ==, 0);

    qtest_clock_step(qts, 1);
    reg_value = qtest_readl(qts, DR_REG_EXTMEM_BASE + reg);
    g_assert_cmphex(reg_value & ena_mask, ==, 0);
    g_assert_cmphex(reg_value & done_mask, ==, done_mask);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_STATE) &
                    idle_mask,
                    ==, idle_mask);
}

static void test_cache_deferred_completion_semantics(void)
{
    QTestState *qts = qts_start();
    const struct {
        uint32_t reg;
        uint32_t trigger_bits;
        uint32_t ena_mask;
        uint32_t done_mask;
        uint32_t idle_mask;
    } cases[] = {
        {
            .reg = A_EXTMEM_DCACHE_SYNC_CTRL,
            .trigger_bits = R_EXTMEM_DCACHE_SYNC_CTRL_INVALIDATE_ENA_MASK,
            .ena_mask = R_EXTMEM_DCACHE_SYNC_CTRL_INVALIDATE_ENA_MASK |
                        R_EXTMEM_DCACHE_SYNC_CTRL_WRITEBACK_ENA_MASK |
                        R_EXTMEM_DCACHE_SYNC_CTRL_CLEAN_ENA_MASK,
            .done_mask = R_EXTMEM_DCACHE_SYNC_CTRL_SYNC_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_DCACHE_STATE_SHIFT,
        },
        {
            .reg = A_EXTMEM_DCACHE_SYNC_CTRL,
            .trigger_bits = R_EXTMEM_DCACHE_SYNC_CTRL_WRITEBACK_ENA_MASK,
            .ena_mask = R_EXTMEM_DCACHE_SYNC_CTRL_INVALIDATE_ENA_MASK |
                        R_EXTMEM_DCACHE_SYNC_CTRL_WRITEBACK_ENA_MASK |
                        R_EXTMEM_DCACHE_SYNC_CTRL_CLEAN_ENA_MASK,
            .done_mask = R_EXTMEM_DCACHE_SYNC_CTRL_SYNC_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_DCACHE_STATE_SHIFT,
        },
        {
            .reg = A_EXTMEM_DCACHE_SYNC_CTRL,
            .trigger_bits = R_EXTMEM_DCACHE_SYNC_CTRL_CLEAN_ENA_MASK,
            .ena_mask = R_EXTMEM_DCACHE_SYNC_CTRL_INVALIDATE_ENA_MASK |
                        R_EXTMEM_DCACHE_SYNC_CTRL_WRITEBACK_ENA_MASK |
                        R_EXTMEM_DCACHE_SYNC_CTRL_CLEAN_ENA_MASK,
            .done_mask = R_EXTMEM_DCACHE_SYNC_CTRL_SYNC_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_DCACHE_STATE_SHIFT,
        },
        {
            .reg = A_EXTMEM_DCACHE_PRELOAD_CTRL,
            .trigger_bits = R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK,
            .ena_mask = R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK,
            .done_mask = R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_DCACHE_STATE_SHIFT,
        },
        {
            .reg = A_EXTMEM_DCACHE_AUTOLOAD_CTRL,
            .trigger_bits = R_EXTMEM_DCACHE_AUTOLOAD_CTRL_AUTOLOAD_ENA_MASK |
                            R_EXTMEM_DCACHE_AUTOLOAD_CTRL_AUTOLOAD_SCT0_ENA_MASK,
            .ena_mask = R_EXTMEM_DCACHE_AUTOLOAD_CTRL_AUTOLOAD_ENA_MASK,
            .done_mask = R_EXTMEM_DCACHE_AUTOLOAD_CTRL_AUTOLOAD_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_DCACHE_STATE_SHIFT,
        },
        {
            .reg = A_EXTMEM_ICACHE_SYNC_CTRL,
            .trigger_bits = R_EXTMEM_ICACHE_SYNC_CTRL_INVALIDATE_ENA_MASK,
            .ena_mask = R_EXTMEM_ICACHE_SYNC_CTRL_INVALIDATE_ENA_MASK,
            .done_mask = R_EXTMEM_ICACHE_SYNC_CTRL_SYNC_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_ICACHE_STATE_SHIFT,
        },
        {
            .reg = A_EXTMEM_ICACHE_PRELOAD_CTRL,
            .trigger_bits = R_EXTMEM_ICACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK,
            .ena_mask = R_EXTMEM_ICACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK,
            .done_mask = R_EXTMEM_ICACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_ICACHE_STATE_SHIFT,
        },
        {
            .reg = A_EXTMEM_ICACHE_AUTOLOAD_CTRL,
            .trigger_bits = R_EXTMEM_ICACHE_AUTOLOAD_CTRL_AUTOLOAD_ENA_MASK |
                            R_EXTMEM_ICACHE_AUTOLOAD_CTRL_AUTOLOAD_SCT0_ENA_MASK,
            .ena_mask = R_EXTMEM_ICACHE_AUTOLOAD_CTRL_AUTOLOAD_ENA_MASK,
            .done_mask = R_EXTMEM_ICACHE_AUTOLOAD_CTRL_AUTOLOAD_DONE_MASK,
            .idle_mask = 1u << R_EXTMEM_CACHE_STATE_ICACHE_STATE_SHIFT,
        },
    };

    for (int i = 0; i < G_N_ELEMENTS(cases); i++) {
        assert_cache_deferred_completion(qts, cases[i].reg,
                                         cases[i].trigger_bits,
                                         cases[i].ena_mask,
                                         cases[i].done_mask,
                                         cases[i].idle_mask);
    }

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_FREEZE,
                 R_EXTMEM_DCACHE_FREEZE_DCACHE_FREEZE_ENA_MASK);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_FREEZE) &
                    R_EXTMEM_DCACHE_FREEZE_DCACHE_FREEZE_DONE_MASK,
                    ==, R_EXTMEM_DCACHE_FREEZE_DCACHE_FREEZE_DONE_MASK);

    qtest_quit(qts);
}

static void test_cache_deferred_ordering_and_clear(void)
{
    QTestState *qts = qts_start();
    const uint32_t dcache_idle =
        1u << R_EXTMEM_CACHE_STATE_DCACHE_STATE_SHIFT;
    const uint32_t icache_idle =
        1u << R_EXTMEM_CACHE_STATE_ICACHE_STATE_SHIFT;
    uint32_t state;

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_SYNC_CTRL,
                 R_EXTMEM_DCACHE_SYNC_CTRL_INVALIDATE_ENA_MASK);
    qtest_clock_step(qts, CACHE_OP_DELAY_NS / 2);
    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_ICACHE_SYNC_CTRL,
                 R_EXTMEM_ICACHE_SYNC_CTRL_INVALIDATE_ENA_MASK);

    qtest_clock_step(qts, CACHE_OP_DELAY_NS / 2 - 1);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_DCACHE_SYNC_CTRL) &
                    R_EXTMEM_DCACHE_SYNC_CTRL_SYNC_DONE_MASK,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_ICACHE_SYNC_CTRL) &
                    R_EXTMEM_ICACHE_SYNC_CTRL_SYNC_DONE_MASK,
                    ==, 0);
    state = qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_STATE);
    g_assert_cmphex(state & dcache_idle, ==, 0);
    g_assert_cmphex(state & icache_idle, ==, 0);

    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_DCACHE_SYNC_CTRL) &
                    R_EXTMEM_DCACHE_SYNC_CTRL_SYNC_DONE_MASK,
                    ==, R_EXTMEM_DCACHE_SYNC_CTRL_SYNC_DONE_MASK);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_ICACHE_SYNC_CTRL) &
                    R_EXTMEM_ICACHE_SYNC_CTRL_SYNC_DONE_MASK,
                    ==, 0);
    state = qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_STATE);
    g_assert_cmphex(state & dcache_idle, ==, dcache_idle);
    g_assert_cmphex(state & icache_idle, ==, 0);

    qtest_clock_step(qts, CACHE_OP_DELAY_NS / 2);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_ICACHE_SYNC_CTRL) &
                    R_EXTMEM_ICACHE_SYNC_CTRL_SYNC_DONE_MASK,
                    ==, R_EXTMEM_ICACHE_SYNC_CTRL_SYNC_DONE_MASK);
    state = qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_STATE);
    g_assert_cmphex(state & dcache_idle, ==, dcache_idle);
    g_assert_cmphex(state & icache_idle, ==, icache_idle);

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_PRELOAD_CTRL,
                 R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_DCACHE_PRELOAD_CTRL) &
                    R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK,
                    ==, 0);
    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_PRELOAD_CTRL, 0);
    qtest_clock_step(qts, CACHE_OP_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_DCACHE_PRELOAD_CTRL) &
                    (R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK |
                     R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK),
                    ==, 0);

    qtest_quit(qts);
}

static void test_cache_fault_preserved_across_deferred_ops(void)
{
    QTestState *qts = qts_start();
    uint32_t fault_content;
    uint32_t fault_vaddr;

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_ENA,
                 R_EXTMEM_CACHE_ILG_INT_ENA_MMU_ENTRY_FAULT_INT_ENA_MASK);
    (void) qtest_readl(qts, ESP32S3_DCACHE_BASE);

    fault_content = qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_CONTENT);
    fault_vaddr = qtest_readl(qts, DR_REG_EXTMEM_BASE +
                              A_EXTMEM_CACHE_MMU_FAULT_VADDR);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_ILG_INT_ST) &
                    R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK,
                    ==, R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK);

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_SYNC_CTRL,
                 R_EXTMEM_DCACHE_SYNC_CTRL_INVALIDATE_ENA_MASK);
    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_DCACHE_PRELOAD_CTRL,
                 R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK);
    qtest_clock_step(qts, CACHE_OP_DELAY_NS);

    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_ILG_INT_ST) &
                    R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK,
                    ==, R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_CONTENT),
                    ==, fault_content);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_VADDR),
                    ==, fault_vaddr);

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_CLR,
                 R_EXTMEM_CACHE_ILG_INT_CLR_MMU_ENTRY_FAULT_INT_CLR_MASK);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_CONTENT),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_VADDR),
                    ==, 0);

    qtest_quit(qts);
}

static void test_cache_invalid_mmu_fault_irq(void)
{
    QTestState *qts = qts_start();
    const uint32_t mmu_entry0 = DR_REG_EXTMEM_BASE + ESP32S3_MMU_TABLE_OFFSET;
    uint32_t fault_content;

    qtest_irq_intercept_in(qts, "/machine/soc/intmatrix");

    g_assert_cmphex(qtest_readl(qts, mmu_entry0), ==, BIT(14));
    g_assert_false(qtest_get_irq(qts, ESP32S3_CACHE_IA_SOURCE));
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_ST),
                    ==, 0);

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_ENA,
                 R_EXTMEM_CACHE_ILG_INT_ENA_MMU_ENTRY_FAULT_INT_ENA_MASK);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_ENA) &
                    R_EXTMEM_CACHE_ILG_INT_ENA_MMU_ENTRY_FAULT_INT_ENA_MASK,
                    ==, R_EXTMEM_CACHE_ILG_INT_ENA_MMU_ENTRY_FAULT_INT_ENA_MASK);

    (void) qtest_readl(qts, ESP32S3_DCACHE_BASE);

    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_ST) &
                    R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK,
                    ==, R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK);
    fault_content = qtest_readl(qts,
                                DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_CONTENT);
    g_assert_cmpuint(FIELD_EX32(fault_content, EXTMEM_CACHE_MMU_FAULT_CONTENT,
                                CACHE_MMU_FAULT_CODE), ==, 1);
    g_assert_cmpuint(FIELD_EX32(fault_content, EXTMEM_CACHE_MMU_FAULT_CONTENT,
                                CACHE_MMU_FAULT_CONTENT), ==, BIT(14));
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_VADDR),
                    ==, ESP32S3_DCACHE_BASE);
    g_assert_true(qtest_get_irq(qts, ESP32S3_CACHE_IA_SOURCE));

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_CLR,
                 R_EXTMEM_CACHE_ILG_INT_CLR_MMU_ENTRY_FAULT_INT_CLR_MASK);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CACHE_ILG_INT_ST) &
                    R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_CONTENT),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE +
                                A_EXTMEM_CACHE_MMU_FAULT_VADDR),
                    ==, 0);
    g_assert_false(qtest_get_irq(qts, ESP32S3_CACHE_IA_SOURCE));

    qtest_quit(qts);
}

static void assert_cache_core0_reject_irq(QTestState *qts, uint32_t access_addr,
                                          uint32_t enable_mask,
                                          uint32_t status_mask,
                                          uint32_t clear_mask,
                                          uint32_t reject_st_addr,
                                          uint32_t reject_vaddr_addr,
                                          uint32_t expected_attr,
                                          uint32_t expected_tag)
{
    uint32_t reject_desc;

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CORE0_ACS_CACHE_INT_ENA,
                 enable_mask);
    g_assert_false(qtest_get_irq(qts, ESP32S3_CACHE_CORE0_ACS_SOURCE));

    qtest_writel(qts, access_addr, 0xa5a5a5a5);

    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CORE0_ACS_CACHE_INT_ST) &
                    status_mask, ==, status_mask);
    reject_desc = qtest_readl(qts, DR_REG_EXTMEM_BASE + reject_st_addr);
    g_assert_cmpuint(extract32(reject_desc, 6, 1), ==, 0);
    g_assert_cmpuint(extract32(reject_desc, 3, 3), ==, expected_attr);
    g_assert_cmpuint(extract32(reject_desc, 0, 3), ==, expected_tag);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + reject_vaddr_addr),
                    ==, access_addr);
    g_assert_true(qtest_get_irq(qts, ESP32S3_CACHE_CORE0_ACS_SOURCE));

    qtest_writel(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CORE0_ACS_CACHE_INT_CLR,
                 clear_mask);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + A_EXTMEM_CORE0_ACS_CACHE_INT_ST) &
                    status_mask, ==, 0);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + reject_st_addr), ==, 0);
    g_assert_cmphex(qtest_readl(qts, DR_REG_EXTMEM_BASE + reject_vaddr_addr),
                    ==, UINT32_MAX);
    g_assert_false(qtest_get_irq(qts, ESP32S3_CACHE_CORE0_ACS_SOURCE));
}

static void test_cache_core0_dbus_reject_irq(void)
{
    g_autofree gchar *flash_path = create_flash_image_with_patterns();
    QTestState *qts = qts_start_with_flash(flash_path);
    const uint32_t mmu_entry0 = DR_REG_EXTMEM_BASE + ESP32S3_MMU_TABLE_OFFSET;

    qtest_irq_intercept_in(qts, "/machine/soc/intmatrix");
    qtest_writel(qts, mmu_entry0, 0);

    assert_cache_core0_reject_irq(qts, ESP32S3_DCACHE_BASE,
                                  R_EXTMEM_CORE0_ACS_CACHE_INT_ENA_CORE0_DBUS_REJECT_INT_ENA_MASK,
                                  R_EXTMEM_CORE0_ACS_CACHE_INT_ST_CORE0_DBUS_REJECT_ST_MASK,
                                  R_EXTMEM_CORE0_ACS_CACHE_INT_CLR_CORE0_DBUS_REJECT_INT_CLR_MASK,
                                  A_EXTMEM_CORE0_DBUS_REJECT_ST,
                                  A_EXTMEM_CORE0_DBUS_REJECT_VADDR,
                                  4, 2);

    qtest_quit(qts);
    unlink(flash_path);
}

static void test_cache_core0_ibus_reject_irq(void)
{
    g_autofree gchar *flash_path = create_flash_image_with_patterns();
    QTestState *qts = qts_start_with_flash(flash_path);
    const uint32_t mmu_entry0 = DR_REG_EXTMEM_BASE + ESP32S3_MMU_TABLE_OFFSET;

    qtest_irq_intercept_in(qts, "/machine/soc/intmatrix");
    qtest_writel(qts, mmu_entry0, 0);

    assert_cache_core0_reject_irq(qts, ESP32S3_ICACHE_BASE,
                                  R_EXTMEM_CORE0_ACS_CACHE_INT_ENA_CORE0_IBUS_REJECT_INT_ENA_MASK,
                                  R_EXTMEM_CORE0_ACS_CACHE_INT_ST_CORE0_IBUS_REJECT_ST_MASK,
                                  R_EXTMEM_CORE0_ACS_CACHE_INT_CLR_CORE0_IBUS_REJECT_INT_CLR_MASK,
                                  A_EXTMEM_CORE0_IBUS_REJECT_ST,
                                  A_EXTMEM_CORE0_IBUS_REJECT_VADDR,
                                  4, 1);

    qtest_quit(qts);
    unlink(flash_path);
}
#endif

static void test_ana_pll_done(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, ANA_BASE + 0x40) & BIT(24), ==, BIT(24));

    qtest_writel(qts, ANA_BASE + 0x44, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, ANA_BASE + 0x44), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_apb_ctrl_register_surface(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, APB_CTRL_DATE_REG), ==,
                    APB_CTRL_DATE_VALUE);
    g_assert_cmphex(qtest_readl(qts, APB_CTRL_QEMU_ORIGIN_REG), ==,
                    APB_CTRL_QEMU_ORIGIN);
    g_assert_cmphex(qtest_readl(qts, APB_CTRL_LEGACY_ECO3_DATE_REG), ==,
                    APB_CTRL_LEGACY_ECO3_DATE);
    g_assert_cmphex(qtest_readl(qts, APB_CTRL_LEGACY_ECO3_DATE_REG) & BIT(31),
                    ==, BIT(31));

    qtest_writel(qts, APB_CTRL_DATE_REG, 0);
    qtest_writel(qts, APB_CTRL_QEMU_ORIGIN_REG, 0);
    qtest_writel(qts, APB_CTRL_LEGACY_ECO3_DATE_REG, 0);
    qtest_writel(qts, APB_CTRL_UNSUPPORTED_REG, 0xffffffff);

    g_assert_cmphex(qtest_readl(qts, APB_CTRL_DATE_REG), ==,
                    APB_CTRL_DATE_VALUE);
    g_assert_cmphex(qtest_readl(qts, APB_CTRL_QEMU_ORIGIN_REG), ==,
                    APB_CTRL_QEMU_ORIGIN);
    g_assert_cmphex(qtest_readl(qts, APB_CTRL_LEGACY_ECO3_DATE_REG), ==,
                    APB_CTRL_LEGACY_ECO3_DATE);
    g_assert_cmphex(qtest_readl(qts, APB_CTRL_UNSUPPORTED_REG), ==, 0);

    qtest_quit(qts);
}

static void test_analog_source_backed_register_shims(void)
{
    QTestState *qts = qts_start();
    const struct {
        uint32_t addr;
        uint32_t reset_value;
        uint32_t write_mask;
    } apb_saradc_cases[] = {
        { APB_SARADC_BASE + 0x00, APB_SARADC_CTRL_DEFAULT,      0xdffffffb },
        { APB_SARADC_BASE + 0x04, APB_SARADC_CTRL2_DEFAULT,     0x01ffffff },
        { APB_SARADC_BASE + 0x18, 0,                            0x00ffffff },
        { APB_SARADC_BASE + 0x28, 0,                            0x00ffffff },
        { APB_SARADC_BASE + 0x38, APB_SARADC_ARB_CTRL_DEFAULT,  0x00001ffc },
        { APB_SARADC_BASE + 0x3c, APB_SARADC_FILTER0_DEFAULT,   0x800fc000 },
        { APB_SARADC_BASE + 0x70, APB_SARADC_CLKM_CONF_DEFAULT, 0x007fffff },
    };
    const struct {
        uint32_t addr;
        uint32_t write_mask;
    } sens_cases[] = {
        { SENS_BASE + 0x10, BIT(31) },
        { SENS_BASE + 0x34, 0xf0000000 },
        { SENS_BASE + 0x3c, 0xe0000000 },
    };
    uint32_t options0;

    for (int i = 0; i < G_N_ELEMENTS(apb_saradc_cases); i++) {
        g_assert_cmphex(qtest_readl(qts, apb_saradc_cases[i].addr), ==,
                        apb_saradc_cases[i].reset_value);
        qtest_writel(qts, apb_saradc_cases[i].addr, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, apb_saradc_cases[i].addr), ==,
                        apb_saradc_cases[i].write_mask);
    }

    for (int i = 0; i < G_N_ELEMENTS(sens_cases); i++) {
        g_assert_cmphex(qtest_readl(qts, sens_cases[i].addr), ==, 0);
        qtest_writel(qts, sens_cases[i].addr, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, sens_cases[i].addr), ==,
                        sens_cases[i].write_mask);
    }

    qtest_writel(qts, APB_SARADC_BASE + 0x74, UINT32_MAX);
    qtest_writel(qts, SENS_BASE + 0x118, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, APB_SARADC_BASE + 0x74), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SENS_BASE + 0x118), ==, 0);

    qtest_writel(qts, APB_SARADC_BASE + 0x00, 0);
    qtest_writel(qts, SENS_BASE + 0x10, UINT32_MAX);
    options0 = FIELD_DP32(0, RTC_CNTL_OPTIONS0, SW_SYS_RESET, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);

    g_assert_cmphex(qtest_readl(qts, APB_SARADC_BASE + 0x00), ==,
                    APB_SARADC_CTRL_DEFAULT);
    g_assert_cmphex(qtest_readl(qts, SENS_BASE + 0x10), ==, 0);

    qtest_quit(qts);
}

static void test_single_cpu_boot(void)
{
    QTestState *qts = qts_start_smp1();

    g_assert_cmphex(qtest_readl(qts, ANA_BASE + 0x40) & BIT(24), ==, BIT(24));

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

static void test_system_clock_register_contract(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_CLOCK_GATE) &
                    R_SYSTEM_CLOCK_GATE_CLK_EN_MASK,
                    ==, R_SYSTEM_CLOCK_GATE_CLK_EN_MASK);

    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_CPU_PER_CONF, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_CPU_PER_CONF),
                    ==, SYSTEM_CPU_PER_CONF_SUPPORTED_MASK);

    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_SYSCLK_CONF, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_SYSCLK_CONF) &
                    ~SYSTEM_SYSCLK_CONF_SUPPORTED_MASK,
                    ==, 0);

    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_PERIP_CLK_EN0, 0x13579bdf);
    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_PERIP_CLK_EN1, 0x2468ace0);
    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_PERIP_RST_EN0, 0x11223344);
    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_PERIP_RST_EN1, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_PERIP_CLK_EN0),
                    ==, 0x13579bdf);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_PERIP_CLK_EN1),
                    ==, 0x2468ace0);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_PERIP_RST_EN0),
                    ==, 0x11223344);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_PERIP_RST_EN1),
                    ==, 0x55667788);

    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_CLOCK_GATE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_CLOCK_GATE),
                    ==, R_SYSTEM_CLOCK_GATE_CLK_EN_MASK);

    qtest_quit(qts);
}

static void test_system_core1_runstall_transition(void)
{
    QTestState *qts = qts_start();

    qtest_irq_intercept_out_named(qts, "/machine/soc/clock",
                                  ESP32S3_CLOCK_CORE1_RUNSTALL_GPIO);

    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_CORE_1_CONTROL_0_REG, 1);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_CORE_1_CONTROL_0_REG),
                    ==, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_CORE_1_CONTROL_0_REG, 0);
    g_assert_cmphex(qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_CORE_1_CONTROL_0_REG),
                    ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_timg_apb_clock_fanout(void)
{
    QTestState *qts = qts_start();
    uint32_t t0_config = 0;
    uint32_t sysclk_conf;

    qtest_writel(qts, TIMG0_BASE + A_TIMG_T0LOADLO, 0);
    qtest_writel(qts, TIMG0_BASE + A_TIMG_T0LOADHI, 0);
    qtest_writel(qts, TIMG0_BASE + A_TIMG_T0LOAD, 1);

    t0_config = FIELD_DP32(t0_config, TIMG_T0CONFIG, DIVIDER, 1);
    t0_config = FIELD_DP32(t0_config, TIMG_T0CONFIG, INCREASE, 1);
    t0_config = FIELD_DP32(t0_config, TIMG_T0CONFIG, EN, 1);
    qtest_writel(qts, TIMG0_BASE + A_TIMG_T0CONFIG, t0_config);

    qtest_clock_step(qts, 1000);
    qtest_writel(qts, TIMG0_BASE + A_TIMG_T0UPDATE,
                 R_TIMG_T0UPDATE_UPDATE_MASK);
    g_assert_cmpuint(qtest_readl(qts, TIMG0_BASE + A_TIMG_T0LO), ==, 80);

    sysclk_conf = qtest_readl(qts, SYSTEM_BASE + A_SYSTEM_SYSCLK_CONF);
    sysclk_conf = FIELD_DP32(sysclk_conf, SYSTEM_SYSCLK_CONF, SOC_CLK_SEL,
                             ESP32S3_CLK_SEL_XTAL);
    qtest_writel(qts, SYSTEM_BASE + A_SYSTEM_SYSCLK_CONF, sysclk_conf);

    qtest_clock_step(qts, 1000);
    qtest_writel(qts, TIMG0_BASE + A_TIMG_T0UPDATE,
                 R_TIMG_T0UPDATE_UPDATE_MASK);
    g_assert_cmpuint(qtest_readl(qts, TIMG0_BASE + A_TIMG_T0LO), ==, 120);

    qtest_quit(qts);
}

static void test_intmatrix_mapping_status_reserved(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, INTMATRIX_MAP_REG(0, ESP32S3_GPIO_SOURCE)),
                    ==, 16);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_MAP_REG(1, ESP32S3_GPIO_SOURCE)),
                    ==, 16);

    qtest_writel(qts, INTMATRIX_MAP_REG(0, ESP32S3_GPIO_SOURCE), 19);
    qtest_writel(qts, INTMATRIX_MAP_REG(1, ESP32S3_GPIO_SOURCE), 20);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_MAP_REG(0, ESP32S3_GPIO_SOURCE)),
                    ==, 19);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_MAP_REG(1, ESP32S3_GPIO_SOURCE)),
                    ==, 20);

    qtest_set_irq_in(qts, "/machine/soc/intmatrix", NULL,
                     ESP32S3_GPIO_SOURCE, 1);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_STATUS_REG(0, 0)) &
                    BIT(ESP32S3_GPIO_SOURCE),
                    ==, BIT(ESP32S3_GPIO_SOURCE));
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_STATUS_REG(1, 0)) &
                    BIT(ESP32S3_GPIO_SOURCE),
                    ==, BIT(ESP32S3_GPIO_SOURCE));

    qtest_set_irq_in(qts, "/machine/soc/intmatrix", NULL,
                     ESP32S3_GPIO_SOURCE, 0);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_STATUS_REG(0, 0)) &
                    BIT(ESP32S3_GPIO_SOURCE),
                    ==, 0);

    qtest_set_irq_in(qts, "/machine/soc/intmatrix", NULL, 15, 1);
    g_assert_cmphex(qtest_readl(qts, INTMATRIX_STATUS_REG(0, 0)) & BIT(15),
                    ==, 0);

    qtest_quit(qts);
}

static void test_rtc_explicit_register_surface(void)
{
    QTestState *qts = qts_start();

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_DATE),
                    ==, ESP32S3_RTC_CNTL_DATE_RESET);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER0, 0x89abcdef);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER0),
                    ==, 0x89abcdef);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER1, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER1),
                    ==, R_RTC_CNTL_SLP_TIMER1_SLP_VAL_HI_MASK);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE),
                    ==, R_RTC_CNTL_WAKEUP_STATE_WAKEUP_ENA_MASK);

    qtest_writel(qts, RTC_EXT_WAKEUP_CONF_REG, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_EXT_WAKEUP_CONF_REG),
                    ==, R_RTC_CNTL_EXT_WAKEUP_CONF_EXT_WAKEUP1_LV_MASK |
                        R_RTC_CNTL_EXT_WAKEUP_CONF_EXT_WAKEUP0_LV_MASK |
                        R_RTC_CNTL_EXT_WAKEUP_CONF_GPIO_WAKEUP_FILTER_MASK);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_REJECT_CONF, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_REJECT_CONF),
                    ==, R_RTC_CNTL_SLP_REJECT_CONF_DEEP_SLP_REJECT_EN_MASK |
                        R_RTC_CNTL_SLP_REJECT_CONF_LIGHT_SLP_REJECT_EN_MASK |
                        R_RTC_CNTL_SLP_REJECT_CONF_SLEEP_REJECT_ENA_MASK);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WDTWPROTECT, 0x50d83aa1);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_WDTWPROTECT),
                    ==, 0x50d83aa1);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SWD_CONF, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SWD_CONF),
                    ==, R_RTC_CNTL_SWD_CONF_SWD_AUTO_FEED_EN_MASK |
                        R_RTC_CNTL_SWD_CONF_SWD_DISABLE_MASK);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SWD_WPROTECT, 0x8f1d312a);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SWD_WPROTECT),
                    ==, 0x8f1d312a);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_PAD_HOLD, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_PAD_HOLD),
                    ==, R_RTC_CNTL_PAD_HOLD_PAD_HOLD_MASK);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_DIG_PAD_HOLD, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_DIG_PAD_HOLD),
                    ==, UINT32_MAX);

    qtest_writel(qts, RTC_EXT_WAKEUP1_REG, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_EXT_WAKEUP1_REG),
                    ==, R_RTC_CNTL_EXT_WAKEUP1_EXT_WAKEUP1_SEL_MASK);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_DATE, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_DATE),
                    ==, R_RTC_CNTL_DATE_DATE_MASK);

    qtest_writel(qts, RTC_RETENTION_CTRL_REG, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RTC_RETENTION_CTRL_REG), ==, 0);

    qtest_quit(qts);
}

static void test_rtc_timer_wakeup_transition(void)
{
    QTestState *qts = qts_start();
    const uint32_t alarm_ticks = 30;
    uint32_t timer1 = 0;
    uint32_t wakeup_state = 0;
    uint32_t state0 = 0;

    qtest_irq_intercept_out_named(qts, "/machine/soc/rtc_cntl",
                                  ESP32S3_RTC_LIGHT_SLEEP_GPIO);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER0, alarm_ticks);
    timer1 = FIELD_DP32(timer1, RTC_CNTL_SLP_TIMER1, MAIN_TIMER_ALARM_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER1, timer1);

    wakeup_state = FIELD_DP32(wakeup_state, RTC_CNTL_WAKEUP_STATE,
                              TIMER_WAKEUP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);

    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLEEP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, state0);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLEEP_EN_MASK,
                    ==, R_RTC_CNTL_STATE0_SLEEP_EN_MASK);
    g_assert_true(qtest_get_irq(qts, 0));

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
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR,
                 R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);

    qtest_quit(qts);
}

static void test_rtc_gpio_low_wakeup_transition(void)
{
    QTestState *qts = qts_start();
    uint32_t pin_cfg = BIT(10) | (GPIO_INT_LOW << GPIO_PIN_INT_TYPE_SHIFT);
    uint32_t wakeup_state = 0;
    uint32_t state0 = 0;

    set_gpio_input_level(qts, 15, true);
    qtest_writel(qts, GPIO_BASE + GPIO_PIN_REG(15), pin_cfg);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);

    wakeup_state = FIELD_DP32(wakeup_state, RTC_CNTL_WAKEUP_STATE,
                              GPIO_WAKEUP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);

    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLEEP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, state0);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLEEP_EN_MASK,
                    ==, R_RTC_CNTL_STATE0_SLEEP_EN_MASK);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);

    set_gpio_input_level(qts, 15, false);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_WAKEUP_CAUSE),
                    ==, R_RTC_CNTL_SLP_WAKEUP_CAUSE_GPIO_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK);

    qtest_quit(qts);
}

static void test_rtc_gpio_low_reject_transition(void)
{
    QTestState *qts = qts_start();
    uint32_t pin_cfg = BIT(10) | (GPIO_INT_LOW << GPIO_PIN_INT_TYPE_SHIFT);
    uint32_t wakeup_state = 0;
    uint32_t reject_conf = 0;
    uint32_t state0 = 0;

    qtest_irq_intercept_out_named(qts, "/machine/soc/rtc_cntl",
                                  ESP32S3_RTC_LIGHT_SLEEP_GPIO);
    g_assert_false(qtest_get_irq(qts, 0));

    set_gpio_input_level(qts, 15, false);
    qtest_writel(qts, GPIO_BASE + GPIO_PIN_REG(15), pin_cfg);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);

    wakeup_state = FIELD_DP32(wakeup_state, RTC_CNTL_WAKEUP_STATE,
                              GPIO_WAKEUP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);

    reject_conf = FIELD_DP32(reject_conf, RTC_CNTL_SLP_REJECT_CONF,
                             LIGHT_SLP_REJECT_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_REJECT_CONF, reject_conf);

    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLEEP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, state0);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_REJECT_MASK,
                    ==, R_RTC_CNTL_INT_RAW_SLP_REJECT_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLP_REJECT_MASK,
                    ==, R_RTC_CNTL_STATE0_SLP_REJECT_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0,
                 R_RTC_CNTL_STATE0_SLP_REJECT_CAUSE_CLR_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_rtc_ext1_low_wakeup_transition(void)
{
    QTestState *qts = qts_start();
    uint32_t state0 = 0;
    uint32_t wakeup_state = RTC_WAKEUP_ENA_EXT1_BIT;
    uint32_t ext_wakeup_conf = 0;

    set_gpio_input_level(qts, 15, true);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);
    qtest_writel(qts, RTC_EXT_WAKEUP1_REG,
                 FIELD_DP32(0, RTC_CNTL_EXT_WAKEUP1, EXT_WAKEUP1_SEL, BIT(15)));
    ext_wakeup_conf = FIELD_DP32(ext_wakeup_conf, RTC_CNTL_EXT_WAKEUP_CONF,
                                 EXT_WAKEUP1_LV, 0);
    qtest_writel(qts, RTC_EXT_WAKEUP_CONF_REG, ext_wakeup_conf);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);

    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLEEP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, state0);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLEEP_EN_MASK,
                    ==, R_RTC_CNTL_STATE0_SLEEP_EN_MASK);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);

    set_gpio_input_level(qts, 15, false);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_WAKEUP_CAUSE),
                    ==, R_RTC_CNTL_SLP_WAKEUP_CAUSE_EXT1_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK);

    qtest_quit(qts);
}

static void test_rtc_ext1_high_wakeup_transition(void)
{
    QTestState *qts = qts_start();
    uint32_t state0 = 0;
    uint32_t wakeup_state = RTC_WAKEUP_ENA_EXT1_BIT;
    uint32_t ext_wakeup_conf = 0;

    set_gpio_input_level(qts, 15, false);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);
    qtest_writel(qts, RTC_EXT_WAKEUP1_REG,
                 FIELD_DP32(0, RTC_CNTL_EXT_WAKEUP1, EXT_WAKEUP1_SEL, BIT(15)));
    ext_wakeup_conf = FIELD_DP32(ext_wakeup_conf, RTC_CNTL_EXT_WAKEUP_CONF,
                                 EXT_WAKEUP1_LV, 1);
    qtest_writel(qts, RTC_EXT_WAKEUP_CONF_REG, ext_wakeup_conf);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);

    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLEEP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, state0);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLEEP_EN_MASK,
                    ==, R_RTC_CNTL_STATE0_SLEEP_EN_MASK);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);

    set_gpio_input_level(qts, 15, true);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_WAKEUP_CAUSE),
                    ==, R_RTC_CNTL_SLP_WAKEUP_CAUSE_EXT1_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK);

    qtest_quit(qts);
}

static void test_rtc_ext1_immediate_wakeup_transition(void)
{
    QTestState *qts = qts_start();
    uint32_t state0 = 0;
    uint32_t wakeup_state = RTC_WAKEUP_ENA_EXT1_BIT;

    set_gpio_input_level(qts, 15, false);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);
    qtest_writel(qts, RTC_EXT_WAKEUP1_REG,
                 FIELD_DP32(0, RTC_CNTL_EXT_WAKEUP1, EXT_WAKEUP1_SEL, BIT(15)));
    qtest_writel(qts, RTC_EXT_WAKEUP_CONF_REG, 0);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);

    state0 = FIELD_DP32(state0, RTC_CNTL_STATE0, SLEEP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, state0);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_WAKEUP_CAUSE),
                    ==, R_RTC_CNTL_SLP_WAKEUP_CAUSE_EXT1_MASK);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0) &
                    R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK,
                    ==, R_RTC_CNTL_STATE0_SLP_WAKEUP_MASK);

    qtest_quit(qts);
}

static void test_rtc_state0_requires_hardware_sleep_bits(void)
{
    QTestState *qts = qts_start();
    const uint32_t alarm_ticks = 30;
    uint32_t timer1 = 0;
    uint32_t wakeup_state = 0;
    uint32_t stale_state0_bits = BIT(29) | BIT(28);

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_CLR, UINT32_MAX);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER0, alarm_ticks);
    timer1 = FIELD_DP32(timer1, RTC_CNTL_SLP_TIMER1, MAIN_TIMER_ALARM_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_TIMER1, timer1);

    wakeup_state = FIELD_DP32(wakeup_state, RTC_CNTL_WAKEUP_STATE,
                              TIMER_WAKEUP_EN, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_WAKEUP_STATE, wakeup_state);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0, stale_state0_bits);

    qtest_clock_step(qts, 300000);

    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_INT_RAW) &
                    R_RTC_CNTL_INT_RAW_SLP_WAKEUP_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_SLP_WAKEUP_CAUSE),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STATE0), ==, 0);

    qtest_quit(qts);
}

static void esp32s3_qmp_system_reset(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(qts, "RESET");
}

static void test_rtc_reset_transitions(void)
{
    QTestState *qts = qts_start();
    const uint32_t scratch_value = 0xfeed1234;
    const uint32_t uart_int_ena = R_UART_INT_ENA_RXFIFO_FULL_MASK |
                                  R_UART_INT_ENA_RXFIFO_TOUT_MASK;
    uint32_t options0 = 0;
    uint32_t reset_state;

    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_STORE0, scratch_value);
    qtest_writel(qts, UART0_BASE + A_UART_INT_ENA, uart_int_ena);

    options0 = FIELD_DP32(options0, RTC_CNTL_OPTIONS0, SW_PROCPU_RESET, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);

    reset_state = qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_RESET_STATE);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_PROCPU), ==, ESP32_SW_CPU_RESET);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_APPCPU), ==, ESP32_POWERON_RESET);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0) &
                    R_RTC_CNTL_OPTIONS0_SW_PROCPU_RESET_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, UART0_BASE + A_UART_INT_ENA),
                    ==, uart_int_ena);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STORE0),
                    ==, scratch_value);

    options0 = FIELD_DP32(0, RTC_CNTL_OPTIONS0, SW_APPCPU_RESET, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);

    reset_state = qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_RESET_STATE);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_PROCPU), ==, ESP32_SW_CPU_RESET);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_APPCPU), ==, ESP32_SW_CPU_RESET);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0) &
                    R_RTC_CNTL_OPTIONS0_SW_APPCPU_RESET_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, UART0_BASE + A_UART_INT_ENA),
                    ==, uart_int_ena);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STORE0),
                    ==, scratch_value);

    options0 = FIELD_DP32(0, RTC_CNTL_OPTIONS0, SW_SYS_RESET, 1);
    qtest_writel(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0, options0);

    reset_state = qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_RESET_STATE);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_PROCPU), ==, ESP32_SW_SYS_RESET);
    g_assert_cmpuint(FIELD_EX32(reset_state, RTC_CNTL_RESET_STATE,
                                RESET_CAUSE_APPCPU), ==, ESP32_SW_SYS_RESET);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_OPTIONS0) &
                    R_RTC_CNTL_OPTIONS0_SW_SYS_RESET_MASK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, UART0_BASE + A_UART_INT_ENA), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RTC_CNTL_BASE + A_RTC_CNTL_STORE0),
                    ==, scratch_value);

    qtest_writel(qts, UART0_BASE + A_UART_INT_ENA, uart_int_ena);
    esp32s3_qmp_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, UART0_BASE + A_UART_INT_ENA), ==, 0);

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
    uint32_t first = qtest_readl(qts, ESP32S3_RNG_BASE);
    bool changed = false;

    for (int i = 0; i < 8; i++) {
        changed |= qtest_readl(qts, ESP32S3_RNG_BASE) != first;
    }

    g_assert_true(changed);
    g_assert_cmphex(qtest_readl(qts, ESP32S3_RNG_BASE + 4), ==, 0);
    qtest_writel(qts, ESP32S3_RNG_BASE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, ESP32S3_RNG_BASE + 4), ==, 0);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readl(qts, ESP32S3_RNG_BASE + 4), ==, 0);

    qtest_quit(qts);
}

static void test_pms_modeled_surface(void)
{
    QTestState *qts = qts_start();
    const uint32_t test_data = 0x12345678;

    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x04), ==, 0xff);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x0c), ==, 1);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x14), ==, 0x7ff);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x2c), ==, 0xf);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x34), ==, 0x3);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x44), ==, 0xfff);
    qtest_writel(qts, PMS_BASE + 0x44, test_data);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x44), ==,
                    test_data & 0xfff);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x200), ==, 0);
    qtest_writel(qts, PMS_BASE + 0x200, test_data);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x200), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x308), ==, 1);
    qtest_writel(qts, PMS_BASE + 0x308, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0x308), ==, 1);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0xffc), ==, PMS_DATE_VALUE);
    qtest_writel(qts, PMS_BASE + 0xffc, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, PMS_BASE + 0xffc), ==, 0x0fffffff);

    qtest_quit(qts);
}

static hwaddr efuse_block_word_addr(unsigned block, unsigned word)
{
    unsigned word_offset;
    unsigned max_words;

    g_assert_cmpuint(block, <=, 10);
    max_words = block < 2 ? ESP_EFUSE_BLOCK0_WORDS : 8;
    g_assert_cmpuint(word, <, max_words);

    if (block == 0) {
        word_offset = 0;
    } else if (block == 1) {
        word_offset = ESP_EFUSE_BLOCK0_WORDS;
    } else {
        word_offset = ESP_EFUSE_BLOCK0_WORDS + ESP_EFUSE_BLOCK1_WORDS +
                      (block - 2) * 8;
    }

    return EFUSE_BASE + A_EFUSE_RD_WR_DIS_REG + (word_offset + word) * 4;
}

static void efuse_clear_ints(QTestState *qts)
{
    qtest_writel(qts, EFUSE_BASE + A_EFUSE_INT_CLR,
                 EFUSE_READ_DONE | EFUSE_PGM_DONE);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_INT_RAW), ==, 0);
}

static void efuse_reload_blocks(QTestState *qts)
{
    qtest_writel(qts, EFUSE_BASE + A_EFUSE_CONF, EFUSE_READ_OPCODE);
    qtest_writel(qts, EFUSE_BASE + A_EFUSE_CMD, BIT(0));
    qtest_clock_step(qts, EFUSE_OP_DELAY_NS);

    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_CMD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_INT_RAW) &
                    EFUSE_READ_DONE, ==, EFUSE_READ_DONE);
    efuse_clear_ints(qts);
}

static void efuse_program_block(QTestState *qts, unsigned block,
                                const uint32_t words[ESP_EFUSE_PGM_DATA_COUNT],
                                bool expect_done)
{
    for (int i = 0; i < ESP_EFUSE_PGM_DATA_COUNT; i++) {
        qtest_writel(qts, EFUSE_BASE + A_EFUSE_PGM_DATA0_REG + i * 4,
                     words[i]);
    }

    qtest_writel(qts, EFUSE_BASE + A_EFUSE_CONF, EFUSE_WRITE_OPCODE);
    qtest_writel(qts, EFUSE_BASE + A_EFUSE_CMD, (block << 2) | BIT(1));
    qtest_clock_step(qts, EFUSE_OP_DELAY_NS);

    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_CMD), ==, 0);
    if (expect_done) {
        g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_INT_RAW) &
                        EFUSE_PGM_DONE, ==, EFUSE_PGM_DONE);
        efuse_clear_ints(qts);
        efuse_reload_blocks(qts);
    } else {
        g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_INT_RAW) &
                        EFUSE_PGM_DONE, ==, 0);
    }
}

static void test_efuse_explicit_contract(void)
{
    QTestState *qts = qts_start();
    const uint32_t block3_first[ESP_EFUSE_PGM_DATA_COUNT] = {
        0x0000ffff, 0x13572468, 0, 0, 0, 0, 0, 0,
    };
    const uint32_t block3_second[ESP_EFUSE_PGM_DATA_COUNT] = {
        0x00ff0000, 0, 0, 0, 0, 0, 0, 0,
    };
    const uint32_t block3_protected[ESP_EFUSE_PGM_DATA_COUNT] = {
        0xff000000, 0xffffffff, 0, 0, 0, 0, 0, 0,
    };
    const uint32_t protect_usr_data[ESP_EFUSE_PGM_DATA_COUNT] = {
        BIT(22), 0, 0, 0, 0, 0, 0, 0,
    };
    const uint32_t protect_key0_read[ESP_EFUSE_PGM_DATA_COUNT] = {
        0, BIT(0), 0, 0, 0, 0, 0, 0,
    };
    const uint32_t block4_key[ESP_EFUSE_PGM_DATA_COUNT] = {
        0x11112222, 0x33334444, 0, 0, 0, 0, 0, 0,
    };

    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_CLK), ==,
                    ESP32S3_EFUSE_CLK_RESET);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_DAC_CONF), ==,
                    ESP32S3_EFUSE_DAC_CONF_RESET);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_RD_TIM_CONF), ==,
                    ESP32S3_EFUSE_RD_TIM_RESET);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_WR_TIM_CONF1), ==,
                    ESP32S3_EFUSE_WR_TIM1_RESET);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_WR_TIM_CONF2), ==,
                    ESP32S3_EFUSE_WR_TIM2_RESET);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_DATE), ==,
                    ESP32S3_EFUSE_DATE_RESET);

    qtest_writel(qts, EFUSE_BASE + A_EFUSE_CLK, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_CLK), ==,
                    ESP32S3_EFUSE_CLK_WR_MASK);
    qtest_writel(qts, EFUSE_BASE + A_EFUSE_DATE, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_DATE), ==,
                    ESP32S3_EFUSE_DATE_WR_MASK);

    efuse_program_block(qts, 3, block3_first, true);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(3, 0)), ==,
                    0x0000ffff);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(3, 1)), ==,
                    0x13572468);

    efuse_program_block(qts, 3, block3_second, true);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(3, 0)), ==,
                    0x00ffffff);

    efuse_program_block(qts, 0, protect_usr_data, true);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(0, 0)) & BIT(22),
                    ==, BIT(22));
    efuse_program_block(qts, 3, block3_protected, false);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(3, 0)), ==,
                    0x00ffffff);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(3, 1)), ==,
                    0x13572468);

    efuse_program_block(qts, 4, block4_key, true);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(4, 0)), ==,
                    0x11112222);
    efuse_program_block(qts, 0, protect_key0_read, true);
    efuse_reload_blocks(qts);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(4, 0)), ==, 0);

    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_CLK), ==,
                    ESP32S3_EFUSE_CLK_RESET);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_DATE), ==,
                    ESP32S3_EFUSE_DATE_RESET);
    g_assert_cmphex(qtest_readl(qts, EFUSE_BASE + A_EFUSE_PGM_DATA0_REG), ==,
                    0);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(3, 0)), ==,
                    0x00ffffff);
    g_assert_cmphex(qtest_readl(qts, efuse_block_word_addr(4, 0)), ==, 0);

    qtest_quit(qts);
}

static uint16_t openeth_mii_read(QTestState *qts, uint8_t reg)
{
    uint32_t cmd = qtest_readl(qts, OPENETH_MIICOMMAND_REG);

    qtest_writel(qts, OPENETH_MIIADDRESS_REG,
                 (reg << OPENETH_MIIADDRESS_RGAD_SHIFT) |
                 (OPENETH_DEFAULT_PHY << OPENETH_MIIADDRESS_FIAD_SHIFT));
    qtest_writel(qts, OPENETH_MIICOMMAND_REG, cmd | OPENETH_MIICOMMAND_RSTAT);

    return qtest_readl(qts, OPENETH_MIIRX_DATA_REG) & 0xffff;
}

static void openeth_read_mac(QTestState *qts, uint8_t mac[6])
{
    uint32_t mac0 = qtest_readl(qts, OPENETH_MAC_ADDR0_REG);
    uint32_t mac1 = qtest_readl(qts, OPENETH_MAC_ADDR1_REG);

    mac[0] = (mac1 >> 8) & 0xff;
    mac[1] = mac1 & 0xff;
    mac[2] = (mac0 >> 24) & 0xff;
    mac[3] = (mac0 >> 16) & 0xff;
    mac[4] = (mac0 >> 8) & 0xff;
    mac[5] = mac0 & 0xff;
}

static void openeth_wait_for_link_state(QTestState *qts, bool up)
{
    uint32_t expected_linkfail = up ? 0 : OPENETH_MIISTATUS_LINKFAIL;

    for (int i = 0; i < 100; i++) {
        uint16_t bmsr = openeth_mii_read(qts, MII_BMSR);
        uint32_t miistatus = qtest_readl(qts, OPENETH_MIISTATUS_REG) &
                             OPENETH_MIISTATUS_LINKFAIL;

        if (!!(bmsr & MII_BMSR_LINK_ST) == up &&
            miistatus == expected_linkfail) {
            return;
        }

        qtest_clock_step(qts, 1);
    }

    g_assert_not_reached();
}

static void test_emac_link_and_loopback_surface(void)
{
    QTestState *qts = qts_start_with_openeth();
    uint8_t mac[6];
    uint8_t tx_frame[64] = { 0 };
    uint8_t rx_frame[sizeof(tx_frame) + 4];
    uint32_t tx_len_flags;
    uint32_t rx_len_flags;

    qtest_irq_intercept_in(qts, "/machine/soc/intmatrix");

    g_assert_cmphex(qtest_readl(qts, EMAC_BASE + 0x00), ==, OPENETH_MODER_DEFAULT);

    qtest_writel(qts, OPENETH_MIICOMMAND_REG, OPENETH_MIICOMMAND_SCANSTAT);
    g_assert_cmphex(qtest_readl(qts, OPENETH_MIICOMMAND_REG) &
                    OPENETH_MIICOMMAND_SCANSTAT,
                    ==, OPENETH_MIICOMMAND_SCANSTAT);
    g_assert_cmphex(openeth_mii_read(qts, MII_BMSR) & MII_BMSR_LINK_ST,
                    ==, MII_BMSR_LINK_ST);
    g_assert_cmphex(qtest_readl(qts, OPENETH_MIISTATUS_REG) &
                    OPENETH_MIISTATUS_LINKFAIL,
                    ==, 0);

    qtest_qmp_assert_success(qts,
                             "{ 'execute': 'set_link', 'arguments': { "
                             "'name': 'emac0', 'up': false } }");
    openeth_wait_for_link_state(qts, false);

    qtest_qmp_assert_success(qts,
                             "{ 'execute': 'set_link', 'arguments': { "
                             "'name': 'emac0', 'up': true } }");
    openeth_wait_for_link_state(qts, true);

    openeth_read_mac(qts, mac);
    memcpy(tx_frame, mac, sizeof(mac));
    tx_frame[6] = 0x02;
    tx_frame[7] = 0x00;
    tx_frame[8] = 0x00;
    tx_frame[9] = 0x00;
    tx_frame[10] = 0x00;
    tx_frame[11] = 0x01;
    tx_frame[12] = 0x08;
    tx_frame[13] = 0x00;
    for (size_t i = 14; i < sizeof(tx_frame); i++) {
        tx_frame[i] = i;
    }

    qtest_memwrite(qts, OPENETH_TX_BUF_BASE, tx_frame, sizeof(tx_frame));
    qtest_memset(qts, OPENETH_RX_BUF_BASE, 0xcc, sizeof(rx_frame));

    qtest_writel(qts, OPENETH_INT_SOURCE_REG, 0xffffffff);
    qtest_writel(qts, OPENETH_INT_MASK_REG,
                 OPENETH_INT_SOURCE_RXB | OPENETH_INT_SOURCE_TXB);

    qtest_writel(qts, OPENETH_RX_DESC0_ADDR + 4, OPENETH_RX_BUF_BASE);
    qtest_writel(qts, OPENETH_RX_DESC0_ADDR + 0,
                 OPENETH_RXD_E | OPENETH_RXD_IRQ);

    qtest_writel(qts, EMAC_BASE + 0x00,
                 OPENETH_MODER_DEFAULT |
                 OPENETH_MODER_LOOPBCK |
                 OPENETH_MODER_PRO |
                 OPENETH_MODER_TXEN |
                 OPENETH_MODER_RXEN);

    qtest_writel(qts, OPENETH_TX_DESC0_ADDR + 4, OPENETH_TX_BUF_BASE);
    qtest_writel(qts, OPENETH_TX_DESC0_ADDR + 0,
                 (sizeof(tx_frame) << 16) |
                 OPENETH_TXD_IRQ |
                 OPENETH_TXD_RD);

    g_assert_true(qtest_get_irq(qts, ESP32S3_EMAC_SOURCE));
    g_assert_cmphex(qtest_readl(qts, OPENETH_INT_SOURCE_REG) &
                    (OPENETH_INT_SOURCE_RXB | OPENETH_INT_SOURCE_TXB),
                    ==, OPENETH_INT_SOURCE_RXB | OPENETH_INT_SOURCE_TXB);

    tx_len_flags = qtest_readl(qts, OPENETH_TX_DESC0_ADDR + 0);
    g_assert_cmphex(tx_len_flags & OPENETH_TXD_RD, ==, 0);

    rx_len_flags = qtest_readl(qts, OPENETH_RX_DESC0_ADDR + 0);
    g_assert_cmphex(rx_len_flags & OPENETH_RXD_E, ==, 0);
    g_assert_cmphex(rx_len_flags & OPENETH_RXD_M, ==, 0);
    g_assert_cmpuint(rx_len_flags >> 16, ==, sizeof(tx_frame) + 4);

    qtest_memread(qts, OPENETH_RX_BUF_BASE, rx_frame, sizeof(rx_frame));
    g_assert_cmpmem(rx_frame, sizeof(tx_frame), tx_frame, sizeof(tx_frame));
    g_assert_cmphex(ldl_le_p(&rx_frame[sizeof(tx_frame)]), ==, 0);

    qtest_writel(qts, OPENETH_INT_SOURCE_REG,
                 OPENETH_INT_SOURCE_RXB | OPENETH_INT_SOURCE_TXB);
    g_assert_false(qtest_get_irq(qts, ESP32S3_EMAC_SOURCE));

    qtest_quit(qts);
}

static void test_emac_not_instantiated_without_nic(void)
{
    QTestState *qts = qts_start_without_openeth();

    assert_openeth_instantiated(qts, false);

    qtest_quit(qts);
}

static void test_emac_instantiated_with_openeth_nic(void)
{
    QTestState *qts = qts_start_with_openeth();

    assert_openeth_instantiated(qts, true);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/esp32s3/ana/pll-done", test_ana_pll_done);
    qtest_add_func("/esp32s3/machine/single-cpu-boot", test_single_cpu_boot);
    qtest_add_func("/esp32s3/generic-mmio/compatibility-storage",
                   test_generic_mmio_compatibility_storage);
    qtest_add_func("/esp32s3/spi0/mem-register-surface",
                   test_spi0_mem_register_surface);
    qtest_add_func("/esp32s3/assist-debug/register-surface",
                   test_assist_debug_register_surface);
    qtest_add_func("/esp32s3/apb-ctrl/register-surface",
                   test_apb_ctrl_register_surface);
    qtest_add_func("/esp32s3/analog/source-backed-register-shims",
                   test_analog_source_backed_register_shims);
#ifndef _WIN32
    qtest_add_func("/esp32s3/cache/flash-mmu-mapping-ctrl1",
                   test_cache_flash_mmu_mapping_and_ctrl1_state);
    qtest_add_func("/esp32s3/cache/psram-mmu-mapping",
                   test_cache_psram_mmu_mapping);
    qtest_add_func("/esp32s3/cache/deferred-completion",
                   test_cache_deferred_completion_semantics);
    qtest_add_func("/esp32s3/cache/deferred-ordering-clear",
                   test_cache_deferred_ordering_and_clear);
    qtest_add_func("/esp32s3/cache/fault-preserved-deferred",
                   test_cache_fault_preserved_across_deferred_ops);
    qtest_add_func("/esp32s3/cache/invalid-mmu-fault-irq",
                   test_cache_invalid_mmu_fault_irq);
    qtest_add_func("/esp32s3/cache/core0-dbus-reject-irq",
                   test_cache_core0_dbus_reject_irq);
    qtest_add_func("/esp32s3/cache/core0-ibus-reject-irq",
                   test_cache_core0_ibus_reject_irq);
#endif
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
    qtest_add_func("/esp32s3/system/clock-register-contract",
                   test_system_clock_register_contract);
    qtest_add_func("/esp32s3/system/core1-runstall",
                   test_system_core1_runstall_transition);
    qtest_add_func("/esp32s3/timg/apb-clock-fanout",
                   test_timg_apb_clock_fanout);
    qtest_add_func("/esp32s3/intmatrix/mapping-status-reserved",
                   test_intmatrix_mapping_status_reserved);
    qtest_add_func("/esp32s3/rtc/explicit-register-surface",
                   test_rtc_explicit_register_surface);
    qtest_add_func("/esp32s3/rtc/timer-wakeup", test_rtc_timer_wakeup_transition);
    qtest_add_func("/esp32s3/rtc/gpio-wakeup", test_rtc_gpio_low_wakeup_transition);
    qtest_add_func("/esp32s3/rtc/gpio-reject", test_rtc_gpio_low_reject_transition);
    qtest_add_func("/esp32s3/rtc/ext1-wakeup", test_rtc_ext1_low_wakeup_transition);
    qtest_add_func("/esp32s3/rtc/ext1-high-wakeup", test_rtc_ext1_high_wakeup_transition);
    qtest_add_func("/esp32s3/rtc/ext1-immediate-wakeup",
                   test_rtc_ext1_immediate_wakeup_transition);
    qtest_add_func("/esp32s3/rtc/state0-hardware-bits", test_rtc_state0_requires_hardware_sleep_bits);
    qtest_add_func("/esp32s3/rtc/reset-transitions", test_rtc_reset_transitions);
    qtest_add_func("/esp32s3/rtc/cpu-stall", test_rtc_cpu_stall_transition);
    qtest_add_func("/esp32s3/uart/clock-timing", test_uart_clock_dependent_timing);
    qtest_add_func("/esp32s3/uart/rx-timeout-multi-config", test_uart_rx_timeout_multi_config);
    qtest_add_func("/esp32s3/sha/irq", test_sha_irq);
    qtest_add_func("/esp32s3/sha/dma-start-continue", test_sha_dma_start_and_continue_irq_paths);
    qtest_add_func("/esp32s3/gpspi/dma-tx-rx-handoff", test_gpspi_dma_txrx_handoff);
    qtest_add_func("/esp32s3/emac/not-instantiated-without-nic",
                   test_emac_not_instantiated_without_nic);
    qtest_add_func("/esp32s3/emac/instantiated-with-openeth-nic",
                   test_emac_instantiated_with_openeth_nic);
    qtest_add_func("/esp32s3/emac/link-loopback", test_emac_link_and_loopback_surface);
    qtest_add_func("/esp32s3/pms/modeled-surface", test_pms_modeled_surface);
    qtest_add_func("/esp32s3/rng/modeled-surface", test_rng_modeled_surface);
    qtest_add_func("/esp32s3/efuse/explicit-contract",
                   test_efuse_explicit_contract);

    return g_test_run();
}
