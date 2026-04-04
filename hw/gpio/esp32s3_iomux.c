/*
 * ESP32-S3 IO_MUX emulation
 *
 * This model only tracks the per-pin function select state that the
 * current firmware path relies on. Pins left in GPIO mode remain eligible
 * for routed board-control signals; switching away from GPIO disables that.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/gpio/esp32s3_iomux.h"

static uint64_t esp32s3_iomux_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(opaque);
    hwaddr index = addr / sizeof(uint32_t);

    if (index >= ESP32S3_IOMUX_REG_COUNT) {
        return 0;
    }

    return s->regs[index];
}

static void esp32s3_iomux_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned int size)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(opaque);
    hwaddr index = addr / sizeof(uint32_t);

    if (index >= ESP32S3_IOMUX_REG_COUNT) {
        return;
    }

    s->regs[index] = (uint32_t)value;

    if (addr >= ESP32S3_IOMUX_GPIO_REG(0) &&
        addr < ESP32S3_IOMUX_GPIO_REG(ESP32S3_GPIO_COUNT) && s->gpio) {
        int pin = (addr / sizeof(uint32_t)) - 1;
        uint32_t func = ((uint32_t)value & ESP32S3_IOMUX_MCU_SEL_MASK) >>
            ESP32S3_IOMUX_MCU_SEL_SHIFT;
        esp32s3_gpio_set_iomux_func(s->gpio, pin, func);
    }
}

static const MemoryRegionOps esp32s3_iomux_ops = {
    .read = esp32s3_iomux_read,
    .write = esp32s3_iomux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_iomux_reset_hold(Object *obj, ResetType type)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(obj);

    memset(s->regs, 0, sizeof(s->regs));

    for (int pin = 0; pin < ESP32S3_GPIO_COUNT; pin++) {
        s->regs[pin + 1] = ESP32S3_GPIO_IOMUX_FUNC_GPIO <<
            ESP32S3_IOMUX_MCU_SEL_SHIFT;
        if (s->gpio) {
            esp32s3_gpio_set_iomux_func(s->gpio, pin,
                                        ESP32S3_GPIO_IOMUX_FUNC_GPIO);
        }
    }
}

static void esp32s3_iomux_init(Object *obj)
{
    ESP32S3IOMuxState *s = ESP32S3_IOMUX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_iomux_ops, s,
                          TYPE_ESP32S3_IOMUX, ESP32S3_IOMUX_IO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void esp32s3_iomux_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_iomux_reset_hold;
}

static const TypeInfo esp32s3_iomux_info = {
    .name = TYPE_ESP32S3_IOMUX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3IOMuxState),
    .instance_init = esp32s3_iomux_init,
    .class_init = esp32s3_iomux_class_init,
};

static void esp32s3_iomux_register_types(void)
{
    type_register_static(&esp32s3_iomux_info);
}

type_init(esp32s3_iomux_register_types)
