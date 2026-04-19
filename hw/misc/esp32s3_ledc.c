#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32s3_ledc.h"

#define ESP32S3_LEDC_REGS_SIZE 0x100

#define ESP32S3_LEDC_LSCH0_CONF0_REG    0x0000
#define ESP32S3_LEDC_LSCH0_HPOINT_REG   0x0004
#define ESP32S3_LEDC_LSCH0_DUTY_REG     0x0008
#define ESP32S3_LEDC_LSCH0_CONF1_REG    0x000c
#define ESP32S3_LEDC_LSTIMER0_CONF_REG  0x00a0
#define ESP32S3_LEDC_CONF_REG           0x00d0
#define ESP32S3_LEDC_DATE_REG           0x00fc

#define ESP32S3_LEDC_LSCH0_CONF0_DEFAULT   0x00000000u
#define ESP32S3_LEDC_LSCH0_CONF1_DEFAULT   0x40000000u
#define ESP32S3_LEDC_LSTIMER0_CONF_DEFAULT 0x00800000u
#define ESP32S3_LEDC_CONF_DEFAULT          0x00000000u
#define ESP32S3_LEDC_DATE_DEFAULT          0x19040200u

static uint32_t esp32s3_ledc_write_mask(hwaddr addr)
{
    switch (addr) {
    case ESP32S3_LEDC_LSCH0_CONF0_REG:
        return 0x0000ffefu;
    case ESP32S3_LEDC_LSCH0_HPOINT_REG:
        return 0x00003fffu;
    case ESP32S3_LEDC_LSCH0_DUTY_REG:
        return 0x0007ffffu;
    case ESP32S3_LEDC_LSCH0_CONF1_REG:
        return 0xffffffffu;
    case ESP32S3_LEDC_LSTIMER0_CONF_REG:
        return 0x01ffffffu;
    case ESP32S3_LEDC_CONF_REG:
        return 0x80000003u;
    case ESP32S3_LEDC_DATE_REG:
        return 0xffffffffu;
    default:
        return 0;
    }
}

static void esp32s3_ledc_reset_regs(ESP32S3LEDCState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[ESP32S3_LEDC_LSCH0_CONF0_REG / sizeof(uint32_t)] =
        ESP32S3_LEDC_LSCH0_CONF0_DEFAULT;
    s->regs[ESP32S3_LEDC_LSCH0_CONF1_REG / sizeof(uint32_t)] =
        ESP32S3_LEDC_LSCH0_CONF1_DEFAULT;
    s->regs[ESP32S3_LEDC_LSTIMER0_CONF_REG / sizeof(uint32_t)] =
        ESP32S3_LEDC_LSTIMER0_CONF_DEFAULT;
    s->regs[ESP32S3_LEDC_CONF_REG / sizeof(uint32_t)] =
        ESP32S3_LEDC_CONF_DEFAULT;
    s->regs[ESP32S3_LEDC_DATE_REG / sizeof(uint32_t)] =
        ESP32S3_LEDC_DATE_DEFAULT;
}

static void esp32s3_ledc_reset(DeviceState *dev)
{
    ESP32S3LEDCState *s = ESP32S3_LEDC(dev);

    esp32s3_ledc_reset_regs(s);
}

static uint64_t esp32s3_ledc_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3LEDCState *s = ESP32S3_LEDC(opaque);
    hwaddr index = addr / sizeof(uint32_t);

    if (index < G_N_ELEMENTS(s->regs)) {
        return s->regs[index];
    }

    return 0;
}

static void esp32s3_ledc_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    ESP32S3LEDCState *s = ESP32S3_LEDC(opaque);
    hwaddr index = addr / sizeof(uint32_t);
    uint32_t mask = esp32s3_ledc_write_mask(addr);

    if (index < G_N_ELEMENTS(s->regs) && mask) {
        s->regs[index] = (uint32_t)value & mask;
    }
}

static const MemoryRegionOps esp32s3_ledc_ops = {
    .read = esp32s3_ledc_read,
    .write = esp32s3_ledc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_ledc_init(Object *obj)
{
    ESP32S3LEDCState *s = ESP32S3_LEDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    esp32s3_ledc_reset_regs(s);
    memory_region_init_io(&s->iomem, obj, &esp32s3_ledc_ops, s,
                          TYPE_ESP32S3_LEDC, ESP32S3_LEDC_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void esp32s3_ledc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, esp32s3_ledc_reset);
}

static const TypeInfo esp32s3_ledc_info = {
    .name = TYPE_ESP32S3_LEDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3LEDCState),
    .instance_init = esp32s3_ledc_init,
    .class_init = esp32s3_ledc_class_init,
};

static void esp32s3_ledc_register_types(void)
{
    type_register_static(&esp32s3_ledc_info);
}

type_init(esp32s3_ledc_register_types)
