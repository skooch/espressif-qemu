/*
 * ESP32S3 Random Number Generator peripheral
 *
 * Copyright (c) 2019-2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/misc/esp32s3_rng.h"


static uint64_t esp32s3_rng_read(void *opaque, hwaddr addr, unsigned int size)
{
    Esp32s3RngState *s = ESP32S3_RNG(opaque);

    if (addr != 0 || size != sizeof(uint32_t)) {
        return 0;
    }

    qemu_guest_getrandom_nofail(&s->last_value, sizeof(s->last_value));
    return s->last_value;
}

static void esp32s3_rng_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    /* WDEV_RND is a read-only data path in the supported QEMU contract. */
}

static const MemoryRegionOps esp32s3_rng_ops = {
    .read =  esp32s3_rng_read,
    .write = esp32s3_rng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_rng_init(Object *obj)
{
    Esp32s3RngState *s = ESP32S3_RNG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_rng_ops, s,
                          TYPE_ESP32S3_RNG, sizeof(uint32_t));
    sysbus_init_mmio(sbd, &s->iomem);
}

static void esp32s3_rng_reset_hold(Object *obj, ResetType type)
{
    Esp32s3RngState *s = ESP32S3_RNG(obj);

    s->last_value = 0;
}

static void esp32s3_rng_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_rng_reset_hold;
}


static const TypeInfo esp32s3_rng_info = {
    .name = TYPE_ESP32S3_RNG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32s3RngState),
    .instance_init = esp32s3_rng_init,
    .class_init = esp32s3_rng_class_init,
};

static void esp32s3_rng_register_types(void)
{
    type_register_static(&esp32s3_rng_info);
}

type_init(esp32s3_rng_register_types)
