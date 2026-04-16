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
#define ESP32S3_PMS_DATE_RESET 0x02101280
#define ESP32S3_PMS_DATE_WR_MASK 0x0fffffff

typedef struct ESP32S3PmsRegInfo {
    hwaddr addr;
    uint32_t reset;
    uint32_t wmask;
} ESP32S3PmsRegInfo;

static const ESP32S3PmsRegInfo esp32s3_pms_regs[] = {
    { 0x000, 0x00000000, 0x00000001 },
    { 0x004, 0x000000ff, 0x000000ff },
    { 0x008, 0x00000000, 0x00000001 },
    { 0x00c, 0x00000001, 0x00000001 },
    { 0x010, 0x00000000, 0x00000001 },
    { 0x014, 0x000007ff, 0x000007ff },
    { 0x018, 0x00000000, 0x0003ffff },
    { 0x01c, 0x00000000, 0x0000000f },
    { 0x020, 0x00000000, 0x0000007f },
    { 0x024, 0x00000000, 0x00000001 },
    { 0x028, 0x00000000, 0x00000001 },
    { 0x02c, 0x0000000f, 0x0000000f },
    { 0x030, 0x00000000, 0x00000001 },
    { 0x034, 0x00000003, 0x00000003 },
    { 0x038, 0x00000000, 0x00000001 },
    { 0x03c, 0x00000fff, 0x00000fff },
    { 0x040, 0x00000000, 0x00000001 },
    { 0x044, 0x00000fff, 0x00000fff },
    { 0x048, 0x00000000, 0x00000001 },
    { 0x04c, 0x00000fff, 0x00000fff },
    { 0x050, 0x00000000, 0x00000001 },
    { 0x054, 0x00000fff, 0x00000fff },
    { 0x058, 0x00000000, 0x00000001 },
    { 0x05c, 0x00000fff, 0x00000fff },
    { 0x060, 0x00000000, 0x00000001 },
    { 0x064, 0x00000fff, 0x00000fff },
    { 0x068, 0x00000000, 0x00000001 },
    { 0x06c, 0x00000fff, 0x00000fff },
    { 0x070, 0x00000000, 0x00000001 },
    { 0x074, 0x00000fff, 0x00000fff },
    { 0x078, 0x00000000, 0x00000001 },
    { 0x07c, 0x00000fff, 0x00000fff },
    { 0x080, 0x00000000, 0x00000001 },
    { 0x084, 0x00000fff, 0x00000fff },
    { 0x088, 0x00000000, 0x00000001 },
    { 0x08c, 0x00000fff, 0x00000fff },
    { 0x090, 0x00000000, 0x00000001 },
    { 0x094, 0x00000fff, 0x00000fff },
    { 0x098, 0x00000000, 0x00000001 },
    { 0x09c, 0x00000fff, 0x00000fff },
    { 0x0a0, 0x00000000, 0x00000001 },
    { 0x308, 0x00000001, 0x00000001 },
    { 0x30c, 0x00000000, 0x00000001 },
    { ESP32S3_PMS_DATE_REG, ESP32S3_PMS_DATE_RESET, ESP32S3_PMS_DATE_WR_MASK },
};

static const ESP32S3PmsRegInfo *esp32s3_pms_find_reg(hwaddr addr)
{
    for (size_t i = 0; i < ARRAY_SIZE(esp32s3_pms_regs); i++) {
        if (esp32s3_pms_regs[i].addr == addr) {
            return &esp32s3_pms_regs[i];
        }
    }

    return NULL;
}

static uint64_t esp32s3_pms_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3PmsState *s = ESP32S3_PMS(opaque);
    uint64_t r = 0;
    const hwaddr index = ESP32S3_PMS_REG_IDX(addr);
    const ESP32S3PmsRegInfo *reg = esp32s3_pms_find_reg(addr);

    if (reg != NULL) {
        r = s->regs[index];
    } else {
#if PMS_WARNING
        warn_report("[PMS] Unsupported read to register %08lx", addr);
#endif
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
    const ESP32S3PmsRegInfo *reg = esp32s3_pms_find_reg(addr);
#if PMS_DEBUG
    warn_report("[PMS]  esp32s3_pms_write addr = %8.8lx, value = %8.8lx, size = %d", addr, value, size);
#endif

    if (reg != NULL) {
        const hwaddr index = ESP32S3_PMS_REG_IDX(addr);
        s->regs[index] = (s->regs[index] & ~reg->wmask) | (value & reg->wmask);
    } else {
#if PMS_WARNING
        warn_report("[PMS] Unsupported write to register %08lx", addr);
#endif
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
    for (size_t i = 0; i < ARRAY_SIZE(esp32s3_pms_regs); i++) {
        const ESP32S3PmsRegInfo *reg = &esp32s3_pms_regs[i];
        s->regs[ESP32S3_PMS_REG_IDX(reg->addr)] = reg->reset;
    }
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
