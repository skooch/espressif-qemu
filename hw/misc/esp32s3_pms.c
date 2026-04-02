/*
 * ESP32-S3 PMS Dummy
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/misc/esp32s3_pms.h"
#include "hw/irq.h"

#define PMS_DEBUG   0
#define PMS_WARNING 0

#define ESP32S3_PMS_REGS_SIZE ESP32S3_PMS_REG_COUNT * sizeof(uint32_t)
#define ESP32S3_PMS_DATE_REG 0xffc
#define ESP32S3_PMS_RW_LIMIT 0x100

static uint64_t esp32s3_pms_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3PmsState *s = ESP32S3_PMS(opaque);
    uint64_t r = 0;
    const hwaddr index = ESP32S3_PMS_REG_IDX(addr);

    switch (addr) {
        case ESP32S3_PMS_DATE_REG:
            r = 0x20260400;
            break;
        default:
            if (addr < ESP32S3_PMS_RW_LIMIT) {
                r = s->regs[index];
            }
#if PMS_WARNING
            warn_report("[PMS] Unsupported read to register %08lx", addr);
#endif
            break;
    }

#if PMS_DEBUG
    info_report("[PMS]  esp32s3_pms_read addr = %8.8lx, value = %8.8lx, size = %d", addr, r, size);
#endif
    return r;
}


static void esp32s3_pms_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    ESP32S3PmsState *s = ESP32S3_PMS(opaque);
#if PMS_DEBUG
    warn_report("[PMS]  esp32s3_pms_write addr = %8.8lx, value = %8.8lx, size = %d", addr, value, size);
#endif
    const hwaddr index = ESP32S3_PMS_REG_IDX(addr);

    switch (addr) {
        default:
            if (addr < ESP32S3_PMS_RW_LIMIT) {
                s->regs[index] = value;
            }
#if PMS_WARNING
            warn_report("[PMS] Unsupported write to register %08lx", addr);
#endif
            break;
    }

}

static const MemoryRegionOps esp32s3_pms_ops = {
    .read =  esp32s3_pms_read,
    .write = esp32s3_pms_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_pms_init(Object *obj)
{
    ESP32S3PmsState *s = ESP32S3_PMS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_pms_ops, s,
                          TYPE_ESP32S3_PMS, ESP32S3_PMS_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

}

static void esp32s3_pms_reset_hold(Object *obj, ResetType type)
{
    ESP32S3PmsState *s = ESP32S3_PMS(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void esp32s3_pms_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_pms_reset_hold;
}

static const TypeInfo esp32s3_pms_info = {
    .name = TYPE_ESP32S3_PMS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3PmsState),
    .instance_init = esp32s3_pms_init,
    .class_init = esp32s3_pms_class_init,
};

static void esp32s3_pms_register_types(void)
{
    type_register_static(&esp32s3_pms_info);
}

type_init(esp32s3_pms_register_types)
