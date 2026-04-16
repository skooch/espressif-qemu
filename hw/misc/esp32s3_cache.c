/*
 * ESP32-S3 ICache emulation
 *
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */


#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32s3_cache.h"
#include "hw/misc/esp32s3_xts_aes.h"
#include "sysemu/block-backend-io.h"
#include "hw/misc/esp32s3_reg.h"


#define CACHE_DEBUG      0
#define CACHE_WARNING    0
#define CACHE_OP_DELAY_NS 1000
#define ESP32S3_CACHE_MMU_FAULT_CPU_MISS 1u
#define ESP32S3_CACHE_MMU_FAULT_ICACHE   (1u << 3)
#define ESP32S3_CACHE_CPU_COUNT          2u
#define ESP32S3_CACHE_ACCESS_ATTR_EXEC   1u
#define ESP32S3_CACHE_ACCESS_ATTR_READ   2u
#define ESP32S3_CACHE_ACCESS_ATTR_WRITE  4u
#define ESP32S3_CACHE_DEADLINE_NONE     (-1)

typedef struct ESP32S3CacheDeferredOp {
    hwaddr addr;
    uint32_t ena_mask;
    uint32_t done_mask;
    bool dcache;
} ESP32S3CacheDeferredOp;

static const ESP32S3CacheDeferredOp esp32s3_cache_deferred_ops[] = {
    {
        .addr = A_EXTMEM_DCACHE_SYNC_CTRL,
        .ena_mask = R_EXTMEM_DCACHE_SYNC_CTRL_INVALIDATE_ENA_MASK |
                    R_EXTMEM_DCACHE_SYNC_CTRL_WRITEBACK_ENA_MASK |
                    R_EXTMEM_DCACHE_SYNC_CTRL_CLEAN_ENA_MASK,
        .done_mask = R_EXTMEM_DCACHE_SYNC_CTRL_SYNC_DONE_MASK,
        .dcache = true,
    },
    {
        .addr = A_EXTMEM_DCACHE_PRELOAD_CTRL,
        .ena_mask = R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK,
        .done_mask = R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK,
        .dcache = true,
    },
    {
        .addr = A_EXTMEM_DCACHE_AUTOLOAD_CTRL,
        .ena_mask = R_EXTMEM_DCACHE_AUTOLOAD_CTRL_AUTOLOAD_ENA_MASK,
        .done_mask = R_EXTMEM_DCACHE_AUTOLOAD_CTRL_AUTOLOAD_DONE_MASK,
        .dcache = true,
    },
    {
        .addr = A_EXTMEM_ICACHE_SYNC_CTRL,
        .ena_mask = R_EXTMEM_ICACHE_SYNC_CTRL_INVALIDATE_ENA_MASK,
        .done_mask = R_EXTMEM_ICACHE_SYNC_CTRL_SYNC_DONE_MASK,
        .dcache = false,
    },
    {
        .addr = A_EXTMEM_ICACHE_PRELOAD_CTRL,
        .ena_mask = R_EXTMEM_ICACHE_PRELOAD_CTRL_PRELOAD_ENA_MASK,
        .done_mask = R_EXTMEM_ICACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK,
        .dcache = false,
    },
    {
        .addr = A_EXTMEM_ICACHE_AUTOLOAD_CTRL,
        .ena_mask = R_EXTMEM_ICACHE_AUTOLOAD_CTRL_AUTOLOAD_ENA_MASK,
        .done_mask = R_EXTMEM_ICACHE_AUTOLOAD_CTRL_AUTOLOAD_DONE_MASK,
        .dcache = false,
    },
};

G_STATIC_ASSERT(ARRAY_SIZE(esp32s3_cache_deferred_ops) ==
                ESP32S3_CACHE_DEFERRED_OP_COUNT);

static size_t esp32s3_cache_deferred_op_index(
    const ESP32S3CacheDeferredOp *op)
{
    return op - esp32s3_cache_deferred_ops;
}

static const ESP32S3CacheDeferredOp *esp32s3_cache_find_deferred_op(hwaddr addr)
{
    for (size_t i = 0; i < ARRAY_SIZE(esp32s3_cache_deferred_ops); i++) {
        if (esp32s3_cache_deferred_ops[i].addr == addr) {
            return &esp32s3_cache_deferred_ops[i];
        }
    }

    return NULL;
}

static bool esp32s3_cache_deferred_op_active(ESP32S3CacheState *s,
                                             size_t op_index)
{
    const ESP32S3CacheDeferredOp *op = &esp32s3_cache_deferred_ops[op_index];
    const hwaddr reg_index = ESP32S3_CACHE_REG_IDX(op->addr);

    return s->completion_deadline_ns[op_index] !=
           ESP32S3_CACHE_DEADLINE_NONE &&
           (s->regs[reg_index] & op->ena_mask) != 0;
}

static bool esp32s3_cache_domain_busy(ESP32S3CacheState *s, bool dcache)
{
    for (size_t i = 0; i < ARRAY_SIZE(esp32s3_cache_deferred_ops); i++) {
        const ESP32S3CacheDeferredOp *op = &esp32s3_cache_deferred_ops[i];

        if (op->dcache == dcache &&
            esp32s3_cache_deferred_op_active(s, i)) {
            return true;
        }
    }

    return false;
}

static const hwaddr esp32s3_cache_access_ena_addrs[ESP32S3_CACHE_CPU_COUNT] = {
    A_EXTMEM_CORE0_ACS_CACHE_INT_ENA,
    A_EXTMEM_CORE1_ACS_CACHE_INT_ENA,
};

static const hwaddr esp32s3_cache_access_st_addrs[ESP32S3_CACHE_CPU_COUNT] = {
    A_EXTMEM_CORE0_ACS_CACHE_INT_ST,
    A_EXTMEM_CORE1_ACS_CACHE_INT_ST,
};

static const hwaddr esp32s3_cache_dbus_reject_st_addrs[ESP32S3_CACHE_CPU_COUNT] = {
    A_EXTMEM_CORE0_DBUS_REJECT_ST,
    A_EXTMEM_CORE1_DBUS_REJECT_ST,
};

static const hwaddr esp32s3_cache_dbus_reject_vaddr_addrs[ESP32S3_CACHE_CPU_COUNT] = {
    A_EXTMEM_CORE0_DBUS_REJECT_VADDR,
    A_EXTMEM_CORE1_DBUS_REJECT_VADDR,
};

static const hwaddr esp32s3_cache_ibus_reject_st_addrs[ESP32S3_CACHE_CPU_COUNT] = {
    A_EXTMEM_CORE0_IBUS_REJECT_ST,
    A_EXTMEM_CORE1_IBUS_REJECT_ST,
};

static const hwaddr esp32s3_cache_ibus_reject_vaddr_addrs[ESP32S3_CACHE_CPU_COUNT] = {
    A_EXTMEM_CORE0_IBUS_REJECT_VADDR,
    A_EXTMEM_CORE1_IBUS_REJECT_VADDR,
};

static const uint32_t esp32s3_cache_dbus_reject_int_masks[ESP32S3_CACHE_CPU_COUNT] = {
    R_EXTMEM_CORE0_ACS_CACHE_INT_ST_CORE0_DBUS_REJECT_ST_MASK,
    R_EXTMEM_CORE1_ACS_CACHE_INT_ST_CORE1_DBUS_REJECT_ST_MASK,
};

static const uint32_t esp32s3_cache_ibus_reject_int_masks[ESP32S3_CACHE_CPU_COUNT] = {
    R_EXTMEM_CORE0_ACS_CACHE_INT_ST_CORE0_IBUS_REJECT_ST_MASK,
    R_EXTMEM_CORE1_ACS_CACHE_INT_ST_CORE1_IBUS_REJECT_ST_MASK,
};

static const uint32_t esp32s3_cache_dbus_reject_clr_masks[ESP32S3_CACHE_CPU_COUNT] = {
    R_EXTMEM_CORE0_ACS_CACHE_INT_CLR_CORE0_DBUS_REJECT_INT_CLR_MASK,
    R_EXTMEM_CORE1_ACS_CACHE_INT_CLR_CORE1_DBUS_REJECT_INT_CLR_MASK,
};

static const uint32_t esp32s3_cache_ibus_reject_clr_masks[ESP32S3_CACHE_CPU_COUNT] = {
    R_EXTMEM_CORE0_ACS_CACHE_INT_CLR_CORE0_IBUS_REJECT_INT_CLR_MASK,
    R_EXTMEM_CORE1_ACS_CACHE_INT_CLR_CORE1_IBUS_REJECT_INT_CLR_MASK,
};

static uint32_t esp32s3_cache_access_core(void)
{
    if (current_cpu != NULL && current_cpu->cpu_index == 1) {
        return 1;
    }

    return 0;
}

static bool esp32s3_cache_bus_enabled(const ESP32S3CacheState *s,
                                      uint32_t core, bool dcache)
{
    const hwaddr ctrl1_addr = dcache ? A_EXTMEM_DCACHE_CTRL1
                                     : A_EXTMEM_ICACHE_CTRL1;
    const uint32_t ctrl1 = s->regs[ESP32S3_CACHE_REG_IDX(ctrl1_addr)];
    uint32_t shut_mask;

    if (dcache) {
        shut_mask = core == 0
            ? R_EXTMEM_DCACHE_CTRL1_SHUT_CORE0_BUS_MASK
            : R_EXTMEM_DCACHE_CTRL1_SHUT_CORE1_BUS_MASK;
    } else {
        shut_mask = core == 0
            ? R_EXTMEM_ICACHE_CTRL1_SHUT_CORE0_BUS_MASK
            : R_EXTMEM_ICACHE_CTRL1_SHUT_CORE1_BUS_MASK;
    }

    return (ctrl1 & shut_mask) == 0;
}

static void esp32s3_cache_update_access_irq(ESP32S3CacheState *s, uint32_t core)
{
    const uint32_t ena =
        s->regs[ESP32S3_CACHE_REG_IDX(esp32s3_cache_access_ena_addrs[core])];
    const uint32_t status =
        s->regs[ESP32S3_CACHE_REG_IDX(esp32s3_cache_access_st_addrs[core])];

    qemu_set_irq(s->access_irq[core], (ena & status) != 0);
}

static void esp32s3_cache_raise_access_reject(ESP32S3CacheState *s,
                                              uint32_t core, bool dcache,
                                              uint32_t vaddr,
                                              uint32_t access_attr,
                                              uint32_t tag_attr)
{
    const hwaddr status_addr = esp32s3_cache_access_st_addrs[core];
    uint32_t reject_desc = 0;

    if (dcache) {
        s->regs[ESP32S3_CACHE_REG_IDX(status_addr)] |=
            esp32s3_cache_dbus_reject_int_masks[core];
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_dbus_reject_vaddr_addrs[core])] = vaddr;
        if (core == 0) {
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE0_DBUS_REJECT_ST,
                                     CORE0_DBUS_ATTR, access_attr);
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE0_DBUS_REJECT_ST,
                                     CORE0_DBUS_TAG_ATTR, tag_attr);
        } else {
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE1_DBUS_REJECT_ST,
                                     CORE1_DBUS_ATTR, access_attr);
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE1_DBUS_REJECT_ST,
                                     CORE1_DBUS_TAG_ATTR, tag_attr);
        }
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_dbus_reject_st_addrs[core])] = reject_desc;
    } else {
        s->regs[ESP32S3_CACHE_REG_IDX(status_addr)] |=
            esp32s3_cache_ibus_reject_int_masks[core];
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_ibus_reject_vaddr_addrs[core])] = vaddr;
        if (core == 0) {
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE0_IBUS_REJECT_ST,
                                     CORE0_IBUS_ATTR, access_attr);
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE0_IBUS_REJECT_ST,
                                     CORE0_IBUS_TAG_ATTR, tag_attr);
        } else {
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE1_IBUS_REJECT_ST,
                                     CORE1_IBUS_ATTR, access_attr);
            reject_desc = FIELD_DP32(reject_desc, EXTMEM_CORE1_IBUS_REJECT_ST,
                                     CORE1_IBUS_TAG_ATTR, tag_attr);
        }
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_ibus_reject_st_addrs[core])] = reject_desc;
    }

    esp32s3_cache_update_access_irq(s, core);
}

static void esp32s3_cache_clear_access_status(ESP32S3CacheState *s,
                                              uint32_t core,
                                              uint32_t clear_mask)
{
    const hwaddr status_idx =
        ESP32S3_CACHE_REG_IDX(esp32s3_cache_access_st_addrs[core]);

    s->regs[status_idx] &= ~clear_mask;

    if (clear_mask & esp32s3_cache_dbus_reject_clr_masks[core]) {
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_dbus_reject_st_addrs[core])] = 0;
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_dbus_reject_vaddr_addrs[core])] = UINT32_MAX;
    }

    if (clear_mask & esp32s3_cache_ibus_reject_clr_masks[core]) {
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_ibus_reject_st_addrs[core])] = 0;
        s->regs[ESP32S3_CACHE_REG_IDX(
            esp32s3_cache_ibus_reject_vaddr_addrs[core])] = UINT32_MAX;
    }

    esp32s3_cache_update_access_irq(s, core);
}

static void esp32s3_cache_update_irq(ESP32S3CacheState *s)
{
    const uint32_t ena =
        s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_ILG_INT_ENA)];
    const uint32_t status =
        s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_ILG_INT_ST)];

    qemu_set_irq(s->illegal_irq, (ena & status) != 0);
}

static void esp32s3_cache_raise_mmu_fault(ESP32S3CacheState *s, uint32_t vaddr,
                                          ESP32S3MMUEntry entry,
                                          uint32_t fault_code)
{
    uint32_t fault_content = 0;

    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_ILG_INT_ST)] |=
        R_EXTMEM_CACHE_ILG_INT_ST_MMU_ENTRY_FAULT_ST_MASK;

    fault_content = FIELD_DP32(fault_content, EXTMEM_CACHE_MMU_FAULT_CONTENT,
                               CACHE_MMU_FAULT_CONTENT, entry.val);
    fault_content = FIELD_DP32(fault_content, EXTMEM_CACHE_MMU_FAULT_CONTENT,
                               CACHE_MMU_FAULT_CODE, fault_code);
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_MMU_FAULT_CONTENT)] =
        fault_content;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_MMU_FAULT_VADDR)] =
        FIELD_DP32(0, EXTMEM_CACHE_MMU_FAULT_VADDR, CACHE_MMU_FAULT_VADDR,
                   vaddr);

    esp32s3_cache_update_irq(s);
}

static void esp32s3_cache_clear_illegal_status(ESP32S3CacheState *s,
                                               uint32_t clear_mask)
{
    const hwaddr status_idx = ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_ILG_INT_ST);

    s->regs[status_idx] &= ~clear_mask;

    if (clear_mask & R_EXTMEM_CACHE_ILG_INT_CLR_MMU_ENTRY_FAULT_INT_CLR_MASK) {
        s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_MMU_FAULT_CONTENT)] = 0;
        s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_MMU_FAULT_VADDR)] = 0;
    }

    esp32s3_cache_update_irq(s);
}

static void esp32s3_cache_complete_deferred_ops(void *opaque)
{
    ESP32S3CacheState *s = ESP32S3_CACHE(opaque);
    const int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next_deadline = INT64_MAX;

    for (size_t i = 0; i < ARRAY_SIZE(esp32s3_cache_deferred_ops); i++) {
        const ESP32S3CacheDeferredOp *op = &esp32s3_cache_deferred_ops[i];
        const hwaddr index = ESP32S3_CACHE_REG_IDX(op->addr);
        const int64_t deadline = s->completion_deadline_ns[i];

        if (deadline == ESP32S3_CACHE_DEADLINE_NONE) {
            continue;
        }

        if ((s->regs[index] & op->ena_mask) == 0) {
            s->completion_deadline_ns[i] = ESP32S3_CACHE_DEADLINE_NONE;
            continue;
        }

        if (deadline <= now) {
            s->regs[index] &= ~op->ena_mask;
            s->regs[index] |= op->done_mask;
            s->completion_deadline_ns[i] = ESP32S3_CACHE_DEADLINE_NONE;
        } else if (deadline < next_deadline) {
            next_deadline = deadline;
        }
    }

    if (next_deadline == INT64_MAX) {
        timer_del(&s->completion_timer);
    } else {
        timer_mod(&s->completion_timer, next_deadline);
    }
}

static void esp32s3_cache_schedule_next_completion(ESP32S3CacheState *s)
{
    int64_t next_deadline = INT64_MAX;

    for (size_t i = 0; i < ARRAY_SIZE(esp32s3_cache_deferred_ops); i++) {
        if (esp32s3_cache_deferred_op_active(s, i) &&
            s->completion_deadline_ns[i] < next_deadline) {
            next_deadline = s->completion_deadline_ns[i];
        }
    }

    if (next_deadline == INT64_MAX) {
        timer_del(&s->completion_timer);
    } else {
        timer_mod(&s->completion_timer, next_deadline);
    }
}

static void esp32s3_cache_write_deferred_op(ESP32S3CacheState *s,
                                            const ESP32S3CacheDeferredOp *op,
                                            uint32_t value)
{
    const size_t op_index = esp32s3_cache_deferred_op_index(op);
    const hwaddr reg_index = ESP32S3_CACHE_REG_IDX(op->addr);
    uint32_t new_value = value & ~op->done_mask;

    if (value & op->ena_mask) {
        s->regs[reg_index] = new_value;
        s->completion_deadline_ns[op_index] =
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CACHE_OP_DELAY_NS;
    } else {
        if (!esp32s3_cache_deferred_op_active(s, op_index)) {
            new_value |= s->regs[reg_index] & op->done_mask;
        }
        s->regs[reg_index] = new_value;
        s->completion_deadline_ns[op_index] =
            ESP32S3_CACHE_DEADLINE_NONE;
    }

    esp32s3_cache_schedule_next_completion(s);
}

static inline uint32_t esp32s3_read_mmu_value(ESP32S3CacheState *s, hwaddr reg_addr)
{
    /* Make the assumption that the address is aligned on sizeof(uint32_t) */
    const uint32_t index = reg_addr / sizeof(uint32_t);
    return (uint32_t) s->mmu[index].val;
}


static void esp32s3_mmu_invalidate_page(ESP32S3CacheState *s, hwaddr virt_addr, hwaddr phys_addr, bool is_psram, bool clear_mr)
{
    AddressSpace *target_as = is_psram
        ? (s->psram != NULL ? &s->psram_as : NULL)
        : (s->flash_blk != NULL ? &s->flash_as : NULL);
    IOMMUTLBEvent event = {
        .type = IOMMU_NOTIFIER_UNMAP,
        .entry = {
            .target_as = target_as,
            .iova = virt_addr,
            .translated_addr = phys_addr,
            .addr_mask = ESP32S3_PAGE_SIZE - 1,
        }
    };
    if (target_as != NULL) {
        memory_region_notify_iommu(&s->dcache_iommu.iommu, 0, event);
        memory_region_notify_iommu(&s->icache_iommu.iommu, 0, event);
    }

    /* If the page was mapped to flash, clear the content */
    if (!is_psram && clear_mr && s->flash_blk != NULL) {
        const uint32_t invalid_value = 0xdeadbeef;
        uint32_t* cache_word_data = (void*) ((uintptr_t) memory_region_get_ram_ptr(&s->flash_mr) + phys_addr);

        for (int i = 0; i < ESP32S3_PAGE_SIZE / sizeof(invalid_value); i++) {
            cache_word_data[i] = invalid_value;
        }
    }
}

void esp32s3_cache_flash_modified(ESP32S3CacheState *s, hwaddr addr,
                                  hwaddr size)
{
    int64_t flash_len;
    hwaddr end;

    if (s == NULL || s->flash_blk == NULL) {
        return;
    }

    flash_len = blk_getlength(s->flash_blk);
    if (flash_len <= 0 || addr >= flash_len) {
        return;
    }

    end = size == 0 ? flash_len : MIN(addr + size, (hwaddr) flash_len);

    for (hwaddr page = addr & ~(ESP32S3_PAGE_SIZE - 1);
         page < end;
         page += ESP32S3_PAGE_SIZE) {
        size_t remaining = flash_len - page;
        size_t load_size = MIN((size_t) ESP32S3_PAGE_SIZE, remaining);
        uint8_t *cache_data =
            ((uint8_t *) memory_region_get_ram_ptr(&s->flash_mr)) + page;

        blk_pread(s->flash_blk, page, load_size, cache_data, 0);
        if (load_size < ESP32S3_PAGE_SIZE) {
            memset(cache_data + load_size, 0xff,
                   ESP32S3_PAGE_SIZE - load_size);
        }

        for (int i = 0; i < ESP32S3_MMU_TABLE_ENTRY_COUNT; i++) {
            const ESP32S3MMUEntry entry = s->mmu[i];

            if (!entry.invalid &&
                entry.type == ESP32S3_MMU_TYPE_FLASH &&
                entry.page_number * ESP32S3_PAGE_SIZE == page) {
                esp32s3_mmu_invalidate_page(s, i * ESP32S3_PAGE_SIZE, page,
                                            false, false);
            }
        }
    }
}


static inline void esp32s3_write_mmu_value(ESP32S3CacheState *s, hwaddr reg_addr, uint32_t value)
{
    ESP32S3XtsAesClass *xts_aes_class = ESP32S3_XTS_AES_GET_CLASS(s->xts_aes);
    /* Make the assumption that the address is aligned on sizeof(uint32_t) */
    const uint32_t index = reg_addr / sizeof(uint32_t);
    /* Reserved bits shall always be 0 */
    ESP32S3MMUEntry e = { .val = value };
    const ESP32S3MMUEntry former = s->mmu[index];
    /* Always keep reserved as 0 */
    e.reserved = 0;
#if CACHE_DEBUG
    info_report("[CACHE] esp32s3_write_mmu_value 0x%lx = %08x, index=%d", reg_addr, value, index);
#endif
    if (former.val != e.val) {
        /* The entry contains the index of the 64KB block from the flash memory */
        const uint32_t physical_address = e.page_number * ESP32S3_PAGE_SIZE;
        const uint32_t former_physaddr = former.page_number * ESP32S3_PAGE_SIZE;
        const uint32_t virtaddr = index * ESP32S3_PAGE_SIZE;
        /* Invalidate the former mapping and clear the MR if and only if this is an "invalidate" operation */
        esp32s3_mmu_invalidate_page(s, virtaddr, former_physaddr, former.type == ESP32S3_MMU_TYPE_PSRAM, e.invalid);

        if (!e.invalid) {
            if (e.type == ESP32S3_MMU_TYPE_FLASH && s->flash_blk != NULL) {
                uint8_t* cache_data = ((uint8_t*) memory_region_get_ram_ptr(&s->flash_mr)) + physical_address;
                blk_pread(s->flash_blk, physical_address, ESP32S3_PAGE_SIZE, cache_data, 0);

                if (xts_aes_class->is_flash_enc_enabled(s->xts_aes)) {
                    xts_aes_class->decrypt(s->xts_aes, physical_address, cache_data, ESP32S3_PAGE_SIZE);
                }
            }
        }
        s->mmu[index].val = e.val;
    }
}


static uint64_t esp32s3_cache_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3CacheState *s = ESP32S3_CACHE(opaque);
    const hwaddr index = ESP32S3_CACHE_REG_IDX(addr);
    uint64_t r = 0;

    if (addr & 0x3) {
        /* Unaligned access, should we fail? */
        error_report("[QEMU] unaligned access to the cache registers");
    }

    switch(addr) {
        case A_EXTMEM_DCACHE_CTRL:
            r = s->dcache_enable;
            break;
        case A_EXTMEM_DCACHE_CTRL1:
            r = s->regs[index];
            break;
        case A_EXTMEM_ICACHE_CTRL:
            r = s->icache_enable;
            break;
        case A_EXTMEM_ICACHE_CTRL1:
            r = s->regs[index];
            break;
        case A_EXTMEM_DCACHE_SYNC_CTRL:
            r = s->regs[index];
            break;
        case A_EXTMEM_ICACHE_SYNC_CTRL:
            r = s->regs[index];
            break;
        case A_EXTMEM_DCACHE_AUTOLOAD_CTRL:
            r = s->regs[index];
            break;
        case A_EXTMEM_ICACHE_AUTOLOAD_CTRL:
            r = s->regs[index];
            break;
        case A_EXTMEM_DCACHE_PRELOAD_CTRL:
            r = s->regs[index];
            break;
        case A_EXTMEM_ICACHE_PRELOAD_CTRL:
            r = s->regs[index];
            break;
        case A_EXTMEM_DCACHE_FREEZE:
            r = s->regs[index];
            break;
        case A_EXTMEM_ICACHE_FREEZE:
            r = s->regs[index];
            break;
        case A_EXTMEM_CACHE_ILG_INT_ENA:
            r = s->regs[index];
            break;
        case A_EXTMEM_CACHE_ILG_INT_ST:
            r = s->regs[index];
            break;
        case A_EXTMEM_CORE0_ACS_CACHE_INT_ENA:
        case A_EXTMEM_CORE0_ACS_CACHE_INT_ST:
        case A_EXTMEM_CORE1_ACS_CACHE_INT_ENA:
        case A_EXTMEM_CORE1_ACS_CACHE_INT_ST:
        case A_EXTMEM_CORE0_DBUS_REJECT_ST:
        case A_EXTMEM_CORE0_DBUS_REJECT_VADDR:
        case A_EXTMEM_CORE0_IBUS_REJECT_ST:
        case A_EXTMEM_CORE0_IBUS_REJECT_VADDR:
        case A_EXTMEM_CORE1_DBUS_REJECT_ST:
        case A_EXTMEM_CORE1_DBUS_REJECT_VADDR:
        case A_EXTMEM_CORE1_IBUS_REJECT_ST:
        case A_EXTMEM_CORE1_IBUS_REJECT_VADDR:
            r = s->regs[index];
            break;
        case A_EXTMEM_CACHE_MMU_FAULT_CONTENT:
            r = s->regs[index];
            break;
        case A_EXTMEM_CACHE_MMU_FAULT_VADDR:
            r = s->regs[index];
            break;
        case A_EXTMEM_CACHE_CONF_MISC:
            r = s->regs[index];
            break;
        case A_EXTMEM_CACHE_STATE:
            if (!esp32s3_cache_domain_busy(s, true)) {
                r |= 1 << R_EXTMEM_CACHE_STATE_DCACHE_STATE_SHIFT;
            }
            if (!esp32s3_cache_domain_busy(s, false)) {
                r |= 1 << R_EXTMEM_CACHE_STATE_ICACHE_STATE_SHIFT;
            }
            break;
        case A_EXTMEM_DCACHE_SYNC_SIZE:
            break;

        case ESP32S3_MMU_TABLE_OFFSET ... (ESP32S3_MMU_TABLE_OFFSET + ESP32S3_MMU_SIZE):
#if CACHE_WARNING
            info_report("[CACHE] Reading 0x%lx (0x%lx)", addr, r);
#endif
            r = esp32s3_read_mmu_value(s, addr - ESP32S3_MMU_TABLE_OFFSET);
            break;
        default:
#if CACHE_WARNING
            warn_report("[CACHE] Unsupported read to 0x%lx", addr);
#endif
            break;
    }

#if CACHE_DEBUG
    info_report("[CACHE] Reading 0x%lx (0x%lx)", addr, r);
#endif

    return r;
}

static void esp32s3_cache_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    ESP32S3CacheState *s = ESP32S3_CACHE(opaque);

    const hwaddr index = ESP32S3_CACHE_REG_IDX(addr);

    if (index < ESP32S3_CACHE_REG_COUNT) {
        switch (addr) {
            case A_EXTMEM_DCACHE_CTRL:
                s->dcache_enable = value & 1;
                break;
            case A_EXTMEM_DCACHE_CTRL1:
                s->regs[index] = value;
                break;
            case A_EXTMEM_ICACHE_CTRL:
                s->icache_enable = value & 1;
                break;
            case A_EXTMEM_ICACHE_CTRL1:
                s->regs[index] = value;
                break;
            case A_EXTMEM_CACHE_ILG_INT_ENA:
                s->regs[index] = value;
                esp32s3_cache_update_irq(s);
                break;
            case A_EXTMEM_CACHE_ILG_INT_CLR:
                esp32s3_cache_clear_illegal_status(s, value);
                break;
            case A_EXTMEM_CORE0_ACS_CACHE_INT_ENA:
                s->regs[index] = value;
                esp32s3_cache_update_access_irq(s, 0);
                break;
            case A_EXTMEM_CORE1_ACS_CACHE_INT_ENA:
                s->regs[index] = value;
                esp32s3_cache_update_access_irq(s, 1);
                break;
            case A_EXTMEM_CORE0_ACS_CACHE_INT_CLR:
                esp32s3_cache_clear_access_status(s, 0, value);
                break;
            case A_EXTMEM_CORE1_ACS_CACHE_INT_CLR:
                esp32s3_cache_clear_access_status(s, 1, value);
                break;
            case A_EXTMEM_ICACHE_FREEZE:
                if (value & R_EXTMEM_ICACHE_FREEZE_ICACHE_FREEZE_ENA_MASK) {
                    /* Enable freeze, set DONE bit */
                    s->regs[index] |= R_EXTMEM_ICACHE_FREEZE_ICACHE_FREEZE_DONE_MASK;
                } else {
                    /* Disable freeze, clear DONE bit */
                    s->regs[index] &= ~R_EXTMEM_ICACHE_FREEZE_ICACHE_FREEZE_DONE_MASK;
                }
                break;
            case A_EXTMEM_DCACHE_FREEZE:
                if (value & R_EXTMEM_DCACHE_FREEZE_DCACHE_FREEZE_ENA_MASK) {
                    /* Enable freeze, set DONE bit */
                    s->regs[index] |= R_EXTMEM_DCACHE_FREEZE_DCACHE_FREEZE_DONE_MASK;
                } else {
                    /* Disable freeze, clear DONE bit */
                    s->regs[index] &= ~R_EXTMEM_DCACHE_FREEZE_DCACHE_FREEZE_DONE_MASK;
                }
                break;
            default:
                {
                    const ESP32S3CacheDeferredOp *op = esp32s3_cache_find_deferred_op(addr);

                    if (op != NULL) {
                        esp32s3_cache_write_deferred_op(s, op, value);
                    } else {
                        s->regs[index] = value;
                    }
                }
                break;
        }
    } else if (addr >= ESP32S3_MMU_TABLE_OFFSET) {
        esp32s3_write_mmu_value(s, addr - ESP32S3_MMU_TABLE_OFFSET, value);
    }

#if CACHE_DEBUG
    info_report("[CACHE] Writing 0x%lx = %08lx, size=%i", addr, value, size);
#endif

}

static const MemoryRegionOps esp32s3_cache_ops = {
    .read =  esp32s3_cache_read,
    .write = esp32s3_cache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_cache_reset_hold(Object *obj, ResetType type)
{
    ESP32S3CacheState *s = ESP32S3_CACHE(obj);
    memset(s->regs, 0, ESP32S3_CACHE_REG_COUNT * sizeof(*s->regs));
    s->icache_enable = false;
    s->dcache_enable = false;
    for (size_t i = 0; i < ARRAY_SIZE(s->completion_deadline_ns); i++) {
        s->completion_deadline_ns[i] = ESP32S3_CACHE_DEADLINE_NONE;
    }
    timer_del(&s->completion_timer);
    qemu_set_irq(s->illegal_irq, 0);
    qemu_set_irq(s->access_irq[0], 0);
    qemu_set_irq(s->access_irq[1], 0);

    /* Initialize the MMU with invalid entries */
    for (int i = 0; i < ESP32S3_MMU_TABLE_ENTRY_COUNT; i++) {
        s->mmu[i].invalid = 1;
    }

    /* On reset, autoload must be set to done (ready) */
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_ICACHE_AUTOLOAD_CTRL)] = R_EXTMEM_ICACHE_AUTOLOAD_CTRL_AUTOLOAD_DONE_MASK;
    /* Same goes for the manual preload */
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_ICACHE_PRELOAD_CTRL)] = R_EXTMEM_ICACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK;

    /* On reset, autoload must be set to done (ready) */
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_DCACHE_AUTOLOAD_CTRL)] = R_EXTMEM_DCACHE_AUTOLOAD_CTRL_AUTOLOAD_DONE_MASK;
    /* Same goes for the manual preload */
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_DCACHE_PRELOAD_CTRL)] = R_EXTMEM_DCACHE_PRELOAD_CTRL_PRELOAD_DONE_MASK;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_DCACHE_CTRL1)] =
        R_EXTMEM_DCACHE_CTRL1_SHUT_CORE0_BUS_MASK |
        R_EXTMEM_DCACHE_CTRL1_SHUT_CORE1_BUS_MASK;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CACHE_CONF_MISC)] =
        R_EXTMEM_CACHE_CONF_MISC_CACHE_IGNORE_SYNC_MMU_ENTRY_FAULT_MASK |
        R_EXTMEM_CACHE_CONF_MISC_CACHE_IGNORE_PRELOAD_MMU_ENTRY_FAULT_MASK;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_ICACHE_CTRL1)] =
        R_EXTMEM_ICACHE_CTRL1_SHUT_CORE0_BUS_MASK |
        R_EXTMEM_ICACHE_CTRL1_SHUT_CORE1_BUS_MASK;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CORE0_DBUS_REJECT_VADDR)] = UINT32_MAX;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CORE0_IBUS_REJECT_VADDR)] = UINT32_MAX;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CORE1_DBUS_REJECT_VADDR)] = UINT32_MAX;
    s->regs[ESP32S3_CACHE_REG_IDX(A_EXTMEM_CORE1_IBUS_REJECT_VADDR)] = UINT32_MAX;
}

static void esp32s3_cache_realize(DeviceState *dev, Error **errp)
{
    /* Initialize the registers */
    esp32s3_cache_reset_hold(OBJECT(dev), RESET_TYPE_COLD);
    ESP32S3CacheState *s = ESP32S3_CACHE(dev);

    /* Make sure XTS_AES was set or issue an error */
    if (s->xts_aes == NULL) {
        error_report("[CACHE] XTS_AES controller must be set!");
    }

    if (s->flash_blk != NULL) {
        /* There is no way to have a MemoryRegion bound to a block device, nor a protable way to have a MemoryRegion
        * region mmap-ed to a file (POSIX systems only). So workaround this by defining some RAM that will be filled
        * with the flash block content every time a map is requested */
        memory_region_init_ram(&s->flash_mr, OBJECT(s), "esp32s3.cache.flash_mr",
                            blk_getlength(s->flash_blk), &error_fatal);

        /* Initialize the address space that will contain the flash MemoryRegion */
        address_space_init(&s->flash_as, &s->flash_mr, "esp32s3.cache.flash_as");
    }

    if (s->psram != NULL) {
        /* Initialize the physical address space for the PSRAM, this will be referenced by the IOMMU. */
        address_space_init(&s->psram_as, &s->psram->data_mr, "esp32s3.cache.psram_as");
    }
}

static void esp32s3_cache_init(Object *obj)
{
    ESP32S3CacheState *s = ESP32S3_CACHE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    timer_init_ns(&s->completion_timer, QEMU_CLOCK_VIRTUAL,
                  esp32s3_cache_complete_deferred_ops, s);

    /* Since the cache I/O region and the MMU I/O region are adjacent, let's use the same MemoryRegion object
     * for both, this will simplify the machine architecture. */
    memory_region_init_io(&s->iomem, obj, &esp32s3_cache_ops, s,
                          TYPE_ESP32S3_CACHE, TYPE_ESP32S3_CACHE_IO_SIZE + ESP32S3_MMU_SIZE);

    /* Initialize separate IOMMU windows so fault reporting can preserve the
     * originating cache alias address. */
    memory_region_init_iommu(&s->dcache_iommu.iommu,
                             sizeof(s->dcache_iommu),
                             TYPE_ESP32S3_MMU_REGION,
                             OBJECT(s),
                             "esp32s3_dcache_iommu",
                             ESP32S3_EXTMEM_REGION_SIZE);
    s->dcache_iommu.cache = s;
    s->dcache_iommu.virt_base = ESP32S3_DCACHE_BASE;
    s->dcache_iommu.dcache = true;

    memory_region_init_iommu(&s->icache_iommu.iommu,
                             sizeof(s->icache_iommu),
                             TYPE_ESP32S3_MMU_REGION,
                             OBJECT(s),
                             "esp32s3_icache_iommu",
                             ESP32S3_EXTMEM_REGION_SIZE);
    s->icache_iommu.cache = s;
    s->icache_iommu.virt_base = ESP32S3_ICACHE_BASE;
    s->icache_iommu.dcache = false;

    /* The Dcache and the Icache are just aliases to the iommu memory region since all the accesses will require
     * to go through a translation. */
    memory_region_init_alias(&s->dcache, OBJECT(s), "esp32s3.dcache",
                           MEMORY_REGION(&s->dcache_iommu.iommu), 0,
                           ESP32S3_EXTMEM_REGION_SIZE);
    memory_region_init_alias(&s->icache, OBJECT(s), "esp32s3.icache",
                           MEMORY_REGION(&s->icache_iommu.iommu), 0,
                           ESP32S3_EXTMEM_REGION_SIZE);

    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->illegal_irq);
    sysbus_init_irq(sbd, &s->access_irq[0]);
    sysbus_init_irq(sbd, &s->access_irq[1]);
}

static Property esp32s3_cache_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_cache_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_cache_reset_hold;
    dc->realize = esp32s3_cache_realize;
    device_class_set_props(dc, esp32s3_cache_properties);
}

static const TypeInfo esp32s3_cache_info = {
    .name = TYPE_ESP32S3_CACHE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3CacheState),
    .instance_init = esp32s3_cache_init,
    .class_init = esp32s3_cache_class_init
};


static uint64_t esp32s3_mmu_region_page_size(IOMMUMemoryRegion *iommu)
{
    return ESP32S3_PAGE_SIZE;
}


/**
 * @brief Function called by the virtual machine when it needs to translate a virtual address (MMU page) into a physical
 * address. Even though we use 64KB pages for the ESP32-S3, this function can still be called several times for the same
 * 64KB page since the host computer's MMU pages are most likely smaller (4KB).
 */
static IOMMUTLBEntry esp32s3_mmu_region_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                                  IOMMUAccessFlags flag, int iommu_idx)
{
    ESP32S3CacheIOMMURegion *region =
        container_of(iommu, ESP32S3CacheIOMMURegion, iommu);
    ESP32S3CacheState *s = region->cache;

    IOMMUTLBEntry ret = {
        .addr_mask = ESP32S3_PAGE_SIZE - 1,
    };

    /* Check which page is being written */
    const uint32_t index = addr / ESP32S3_PAGE_SIZE;
    const uint32_t offset = addr % ESP32S3_PAGE_SIZE;
    const ESP32S3MMUEntry entry = s->mmu[index];
    const uint32_t vaddr = region->virt_base + addr;

    /* Make sure the virtual and physical addresses are both aligned on ESP32S3_PAGE_SIZE when returned to the caller */
    ret.translated_addr = entry.page_number * ESP32S3_PAGE_SIZE;
    ret.iova = addr - offset;

    if (entry.invalid) {
        const uint32_t fault_code = ESP32S3_CACHE_MMU_FAULT_CPU_MISS |
            (region->dcache ? 0 : ESP32S3_CACHE_MMU_FAULT_ICACHE);

        esp32s3_cache_raise_mmu_fault(s, vaddr, entry, fault_code);
        ret.perm = IOMMU_NONE;
        return ret;
    }

    if (!esp32s3_cache_bus_enabled(s, esp32s3_cache_access_core(),
                                   region->dcache)) {
        ret.perm = IOMMU_NONE;
        return ret;
    }

    if ((flag & IOMMU_WO) && entry.type == ESP32S3_MMU_TYPE_FLASH) {
        const uint32_t core = esp32s3_cache_access_core();

        esp32s3_cache_raise_access_reject(s, core, region->dcache, vaddr,
                                          ESP32S3_CACHE_ACCESS_ATTR_WRITE,
                                          region->dcache
                                              ? ESP32S3_CACHE_ACCESS_ATTR_READ
                                              : ESP32S3_CACHE_ACCESS_ATTR_EXEC);
        ret.perm = IOMMU_NONE;
        return ret;
    }

    if (entry.type == ESP32S3_MMU_TYPE_PSRAM) {
        /* If there is no PSRAM connected to the machine, give no permission to the address space */
        if (s->psram == NULL) {
            ret.perm = IOMMU_NONE;
        } else {
            ret.target_as = &s->psram_as;
            ret.perm = IOMMU_RW;
        }
    } else {
        if (s->flash_blk == NULL) {
            ret.perm = IOMMU_NONE;
        } else {
            ret.target_as = &s->flash_as;
            ret.perm = IOMMU_RO;
        }
    }

#if CACHE_DEBUG
    info_report("[Cache] Translate virtual address %08lx to %08lx (idx: %d, val: %x, offset: %08x)\x1b[0m\n", ret.iova, ret.translated_addr, index, entry.val, offset);
#endif
    return ret;
}

static int esp32s3_mmu_region_attrs_to_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    return 0;
}


static int esp32s3_mmu_region_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                                  IOMMUNotifierFlag old,
                                                  IOMMUNotifierFlag new,
                                                  Error **errp)
{
    return 0;
}


static void esp32s3_mmu_region_class_init(ObjectClass *klass, void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = esp32s3_mmu_region_translate;
    imrc->attrs_to_index = esp32s3_mmu_region_attrs_to_index;
    imrc->get_min_page_size = esp32s3_mmu_region_page_size;
    imrc->notify_flag_changed = esp32s3_mmu_region_notify_flag_changed;
}

static const TypeInfo esp32s3_mmu_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_ESP32S3_MMU_REGION,
    .class_init = esp32s3_mmu_region_class_init,
};


static void esp32s3_cache_register_types(void)
{
    type_register_static(&esp32s3_cache_info);
    type_register_static(&esp32s3_mmu_region_info);
}

type_init(esp32s3_cache_register_types)
