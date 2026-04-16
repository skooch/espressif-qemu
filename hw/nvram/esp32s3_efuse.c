/*
 * ESP32-S3 eFuse emulation
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/nvram/esp32s3_efuse.h"

static void esp32s3_efuse_apply_reset_defaults(ESPEfuseState *s)
{
    s->efuses.clk = ESP32S3_EFUSE_CLK_RESET;
    s->efuses.conf = 0;
    s->efuses.dac_conf = ESP32S3_EFUSE_DAC_CONF_RESET;
    s->efuses.rd_tim_conf = ESP32S3_EFUSE_RD_TIM_RESET;
    s->efuses.wr_tim_conf1 = ESP32S3_EFUSE_WR_TIM1_RESET;
    s->efuses.wr_tim_conf2 = ESP32S3_EFUSE_WR_TIM2_RESET;
    s->efuses.date = ESP32S3_EFUSE_DATE_RESET;
}

static bool esp32s3_efuse_get_key(ESPEfuseState *s,
                                  EfuseBlocksIdx efuse_block_num,
                                  uint8_t *efuse_key)
{
    if (efuse_block_num < EFUSE_BLOCK_KEY0 || efuse_block_num > EFUSE_BLOCK_KEY5) {
        error_report("[Efuse] ESP32-S3 key block out of range: %d",
                     efuse_block_num);
        return false;
    }

    uint8_t *block_key0 = (uint8_t *) &s->efuses_internal.blocks.rd_key0_data0;
    memcpy(efuse_key, block_key0 + (efuse_block_num - EFUSE_BLOCK_KEY0) * 32,
           32);
    return true;
}

static uint32_t esp32s3_efuse_get_key_purpose(ESPEfuseState *s,
                                              EfuseBlocksIdx efuse_block_num)
{
    switch (efuse_block_num) {
    case EFUSE_BLOCK_KEY0:
        return FIELD_EX32(s->efuses.blocks.rd_repeat_data1,
                          EFUSE_RD_REPEAT_DATA1, KEY_PURP_0);
    case EFUSE_BLOCK_KEY1:
        return FIELD_EX32(s->efuses.blocks.rd_repeat_data1,
                          EFUSE_RD_REPEAT_DATA1, KEY_PURP_1);
    case EFUSE_BLOCK_KEY2:
        return FIELD_EX32(s->efuses.blocks.rd_repeat_data2,
                          EFUSE_RD_REPEAT_DATA2, KEY_PURP_2);
    case EFUSE_BLOCK_KEY3:
        return FIELD_EX32(s->efuses.blocks.rd_repeat_data2,
                          EFUSE_RD_REPEAT_DATA2, KEY_PURP_3);
    case EFUSE_BLOCK_KEY4:
        return FIELD_EX32(s->efuses.blocks.rd_repeat_data2,
                          EFUSE_RD_REPEAT_DATA2, KEY_PURP_4);
    case EFUSE_BLOCK_KEY5:
        return FIELD_EX32(s->efuses.blocks.rd_repeat_data2,
                          EFUSE_RD_REPEAT_DATA2, KEY_PURP_5);
    default:
        error_report("[Efuse] ESP32-S3 key purpose block out of range: %d",
                     efuse_block_num);
        return UINT32_MAX;
    }
}

static void esp32s3_efuse_realize(DeviceState *dev, Error **errp)
{
    ESP32S3EfuseClass* esp32s3_class = ESP32S3_EFUSE_GET_CLASS(dev);

    esp32s3_class->parent_realize(dev, errp);
}


static void esp32s3_efuse_init(Object *obj)
{
}

static void esp32s3_efuse_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ESP32S3EfuseClass* esp32s3_efuse = ESP32S3_EFUSE_CLASS(klass);
    ESPEfuseClass *esp_efuse = ESP_EFUSE_CLASS(klass);

    device_class_set_parent_realize(dc, esp32s3_efuse_realize, &esp32s3_efuse->parent_realize);
    esp_efuse->apply_reset_defaults = esp32s3_efuse_apply_reset_defaults;
    esp_efuse->get_key = esp32s3_efuse_get_key;
    esp_efuse->get_key_purpose = esp32s3_efuse_get_key_purpose;
}

static const TypeInfo esp32s3_efuse_info = {
    .name = TYPE_ESP32S3_EFUSE,
    .parent = TYPE_ESP_EFUSE,
    .instance_size = sizeof(ESP32S3EfuseState),
    .instance_init = esp32s3_efuse_init,
    .class_init = esp32s3_efuse_class_init,
    .class_size = sizeof(ESP32S3EfuseClass)
};

static void esp32s3_efuse_register_types(void)
{
    type_register_static(&esp32s3_efuse_info);
}

type_init(esp32s3_efuse_register_types)
