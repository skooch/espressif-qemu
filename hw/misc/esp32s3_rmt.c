#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/bitops.h"
#include "hw/misc/esp32s3_rmt.h"

#define ESP32S3_RMT_REGS_SIZE          0x1000
#define ESP32S3_RMT_MODELED_SIZE       0x00d0

#define ESP32S3_RMT_CHNDATA_BASE       0x0000
#define ESP32S3_RMT_CHMDATA_BASE       0x0010
#define ESP32S3_RMT_CHNCONF0_BASE      0x0020
#define ESP32S3_RMT_CHMCONF_BASE       0x0030
#define ESP32S3_RMT_CHNSTATUS_BASE     0x0050
#define ESP32S3_RMT_CHMSTATUS_BASE     0x0060
#define ESP32S3_RMT_INT_RAW_REG        0x0070
#define ESP32S3_RMT_INT_ST_REG         0x0074
#define ESP32S3_RMT_INT_ENA_REG        0x0078
#define ESP32S3_RMT_INT_CLR_REG        0x007c
#define ESP32S3_RMT_CHNCARRIER_BASE    0x0080
#define ESP32S3_RMT_CHM_RXCARRIER_BASE 0x0090
#define ESP32S3_RMT_CHN_TX_LIM_BASE    0x00a0
#define ESP32S3_RMT_CHM_RX_LIM_BASE    0x00b0
#define ESP32S3_RMT_SYS_CONF_REG       0x00c0
#define ESP32S3_RMT_TX_SIM_REG         0x00c4
#define ESP32S3_RMT_REF_CNT_RST_REG    0x00c8
#define ESP32S3_RMT_DATE_REG           0x00cc

#define ESP32S3_RMT_CHNCONF0_DEFAULT      0x00710200u
#define ESP32S3_RMT_CHMCONF0_DEFAULT      0x31007f02u
#define ESP32S3_RMT_CHMCONF1_DEFAULT      0x000001e8u
#define ESP32S3_RMT_CHMSTATUS_DEFAULT     0x000600c0u
#define ESP32S3_RMT_CHNCARRIER_DEFAULT    0x00400040u
#define ESP32S3_RMT_CHN_TX_LIM_DEFAULT    0x00000080u
#define ESP32S3_RMT_CHM_RX_LIM_DEFAULT    0x00000080u
#define ESP32S3_RMT_SYS_CONF_DEFAULT      0x05000010u
#define ESP32S3_RMT_DATE_DEFAULT          0x02101181u

#define ESP32S3_RMT_INT_MASK              0x3fffffffu

static uint32_t esp32s3_rmt_mask(hwaddr addr)
{
    if (addr >= ESP32S3_RMT_CHNDATA_BASE && addr < ESP32S3_RMT_CHNDATA_BASE + 0x10) {
        return 0xffffffffu;
    }
    if (addr >= ESP32S3_RMT_CHMDATA_BASE && addr < ESP32S3_RMT_CHMDATA_BASE + 0x10) {
        return 0xffffffffu;
    }
    if (addr >= ESP32S3_RMT_CHNCONF0_BASE && addr < ESP32S3_RMT_CHNCONF0_BASE + 0x10) {
        return 0x007ffff8u;
    }
    if (addr >= ESP32S3_RMT_CHMCONF_BASE && addr < ESP32S3_RMT_CHMCONF_BASE + 0x20) {
        return ((addr - ESP32S3_RMT_CHMCONF_BASE) & 0x4) ? 0x00003ff9u : 0x3f7fffffu;
    }
    if (addr >= ESP32S3_RMT_CHNCARRIER_BASE && addr < ESP32S3_RMT_CHNCARRIER_BASE + 0x10) {
        return 0xffffffffu;
    }
    if (addr >= ESP32S3_RMT_CHM_RXCARRIER_BASE &&
        addr < ESP32S3_RMT_CHM_RXCARRIER_BASE + 0x10) {
        return 0xffffffffu;
    }
    if (addr >= ESP32S3_RMT_CHN_TX_LIM_BASE && addr < ESP32S3_RMT_CHN_TX_LIM_BASE + 0x10) {
        return 0x002fffffu;
    }
    if (addr >= ESP32S3_RMT_CHM_RX_LIM_BASE && addr < ESP32S3_RMT_CHM_RX_LIM_BASE + 0x10) {
        return 0x000001ffu;
    }

    switch (addr) {
    case ESP32S3_RMT_INT_ENA_REG:
        return ESP32S3_RMT_INT_MASK;
    case ESP32S3_RMT_SYS_CONF_REG:
        return 0x87ffffffu;
    case ESP32S3_RMT_TX_SIM_REG:
        return 0x0000001fu;
    case ESP32S3_RMT_DATE_REG:
        return 0x0fffffffu;
    default:
        return 0;
    }
}

static inline uint32_t esp32s3_rmt_reg_index(hwaddr addr)
{
    return addr / sizeof(uint32_t);
}

static void esp32s3_rmt_update_irq(ESP32S3RMTState *s)
{
    qemu_set_irq(s->irq, (s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_INT_RAW_REG)] &
                          s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_INT_ENA_REG)]) != 0);
}

static void esp32s3_rmt_reset_regs(ESP32S3RMTState *s)
{
    memset(s->regs, 0, sizeof(s->regs));

    for (int i = 0; i < 4; i++) {
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_CHNCONF0_BASE + i * 4)] =
            ESP32S3_RMT_CHNCONF0_DEFAULT;
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_CHMCONF_BASE + i * 8)] =
            ESP32S3_RMT_CHMCONF0_DEFAULT;
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_CHMCONF_BASE + i * 8 + 4)] =
            ESP32S3_RMT_CHMCONF1_DEFAULT;
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_CHMSTATUS_BASE + i * 4)] =
            ESP32S3_RMT_CHMSTATUS_DEFAULT;
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_CHNCARRIER_BASE + i * 4)] =
            ESP32S3_RMT_CHNCARRIER_DEFAULT;
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_CHN_TX_LIM_BASE + i * 4)] =
            ESP32S3_RMT_CHN_TX_LIM_DEFAULT;
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_CHM_RX_LIM_BASE + i * 4)] =
            ESP32S3_RMT_CHM_RX_LIM_DEFAULT;
    }

    s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_SYS_CONF_REG)] =
        ESP32S3_RMT_SYS_CONF_DEFAULT;
    s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_DATE_REG)] =
        ESP32S3_RMT_DATE_DEFAULT;

    esp32s3_rmt_update_irq(s);
}

static void esp32s3_rmt_trigger_tx_channel(ESP32S3RMTState *s, unsigned channel)
{
    uint32_t tx_sim = s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_TX_SIM_REG)];
    uint32_t channels = BIT(channel);

    if (tx_sim & BIT(4)) {
        channels |= tx_sim & 0x0fu;
    }

    s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_INT_RAW_REG)] |= channels & 0x0fu;
    esp32s3_rmt_update_irq(s);
}

static void esp32s3_rmt_reset(DeviceState *dev)
{
    ESP32S3RMTState *s = ESP32S3_RMT(dev);

    esp32s3_rmt_reset_regs(s);
}

static uint64_t esp32s3_rmt_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3RMTState *s = ESP32S3_RMT(opaque);

    if (size != sizeof(uint32_t) || addr >= ESP32S3_RMT_REGS_SIZE) {
        return 0;
    }

    if (addr == ESP32S3_RMT_INT_ST_REG) {
        return s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_INT_RAW_REG)] &
               s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_INT_ENA_REG)];
    }
    if (addr == ESP32S3_RMT_INT_CLR_REG || addr == ESP32S3_RMT_REF_CNT_RST_REG) {
        return 0;
    }
    if (addr < ESP32S3_RMT_MODELED_SIZE) {
        return s->regs[esp32s3_rmt_reg_index(addr)];
    }

    return 0;
}

static void esp32s3_rmt_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    ESP32S3RMTState *s = ESP32S3_RMT(opaque);
    uint32_t val = (uint32_t)value;
    uint32_t index;
    uint32_t mask;

    if (size != sizeof(uint32_t) || addr >= ESP32S3_RMT_REGS_SIZE) {
        return;
    }

    switch (addr) {
    case ESP32S3_RMT_INT_RAW_REG:
    case ESP32S3_RMT_INT_CLR_REG:
        s->regs[esp32s3_rmt_reg_index(ESP32S3_RMT_INT_RAW_REG)] &=
            ~(val & ESP32S3_RMT_INT_MASK);
        esp32s3_rmt_update_irq(s);
        return;
    case ESP32S3_RMT_INT_ENA_REG:
        s->regs[esp32s3_rmt_reg_index(addr)] = val & ESP32S3_RMT_INT_MASK;
        esp32s3_rmt_update_irq(s);
        return;
    case ESP32S3_RMT_REF_CNT_RST_REG:
        return;
    default:
        break;
    }

    if (addr >= ESP32S3_RMT_CHNSTATUS_BASE &&
        addr < ESP32S3_RMT_CHMSTATUS_BASE + 0x10) {
        return;
    }

    mask = esp32s3_rmt_mask(addr);
    if (!mask || addr >= ESP32S3_RMT_MODELED_SIZE) {
        return;
    }

    index = esp32s3_rmt_reg_index(addr);
    s->regs[index] = val & mask;

    if (addr >= ESP32S3_RMT_CHNCONF0_BASE && addr < ESP32S3_RMT_CHNCONF0_BASE + 0x10) {
        unsigned channel = (addr - ESP32S3_RMT_CHNCONF0_BASE) / 4;

        if (val & BIT(0)) {
            s->regs[index] &= ~BIT(7);
            esp32s3_rmt_trigger_tx_channel(s, channel);
        }
    }
}

static const MemoryRegionOps esp32s3_rmt_ops = {
    .read = esp32s3_rmt_read,
    .write = esp32s3_rmt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_rmt_init(Object *obj)
{
    ESP32S3RMTState *s = ESP32S3_RMT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    esp32s3_rmt_reset_regs(s);
    memory_region_init_io(&s->iomem, obj, &esp32s3_rmt_ops, s,
                          TYPE_ESP32S3_RMT, ESP32S3_RMT_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void esp32s3_rmt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, esp32s3_rmt_reset);
}

static const TypeInfo esp32s3_rmt_info = {
    .name = TYPE_ESP32S3_RMT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3RMTState),
    .instance_init = esp32s3_rmt_init,
    .class_init = esp32s3_rmt_class_init,
};

static void esp32s3_rmt_register_types(void)
{
    type_register_static(&esp32s3_rmt_info);
}

type_init(esp32s3_rmt_register_types)
