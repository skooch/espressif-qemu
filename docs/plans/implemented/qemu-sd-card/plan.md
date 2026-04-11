# QEMU SD Card SPI Emulation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add SD card emulation to the QEMU T-Deck Pro emulator via an SPI slave model wrapping QEMU's built-in SDState, with control panel insert/eject.

**Architecture:** New `tdeck_sd_spi.c` implements SPI-to-SD protocol translation. GP-SPI routes SPI bytes to SD when GPIO48 (CS) is LOW, using existing GDMA RX path for MISO data. SDState provides all SD command/block handling backed by a host file or vvfat directory.

**Tech Stack:** C (QEMU device models), QEMU SD card subsystem (`hw/sd/sd.h`)

**Spec:** `docs/plans/new/qemu-sd-card/spec.md`

**QEMU fork:** `/Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3/` (branch `tdeck-peripherals`)

---

## File Map

- **Create:** `hw/ssi/tdeck_sd_spi.c` — SD SPI slave device model (protocol state machine)
- **Create:** `include/hw/ssi/tdeck_sd_spi.h` — public type, state struct, transfer API
- **Modify:** `hw/ssi/esp32s3_gpspi.c` — GPIO48 CS routing + GDMA RX writeback for SD
- **Modify:** `include/hw/ssi/esp32s3_gpspi.h` — add `sd_spi` field to GP-SPI state
- **Modify:** `hw/display/tdeck_uc8253.c` — control panel SD insert/eject button
- **Modify:** `include/hw/display/tdeck_uc8253.h` — add `sd_spi` pointer and `panel_sd` field
- **Modify:** `hw/xtensa/esp32s3.c` — create SD device, wire into GP-SPI and panel
- **Modify:** `include/hw/xtensa/esp32s3.h` — add `sd_spi` field to SoC state
- **Modify:** `hw/ssi/meson.build` — add `tdeck_sd_spi.c` to build
- **Create:** `scripts/qemu-sdcard.sh` — FAT32 image creation helper
- **Modify:** `scripts/qemu-run.sh` — add `--sd` flag
- **Modify:** `scripts/qemu-e2e.sh` — add SD card test assertions

---

## Task 1: SD SPI Protocol Model — Header

**Files:**
- Create: `include/hw/ssi/tdeck_sd_spi.h`

- [ ] **Step 1: Create the header file**

```c
/*
 * T-Deck Pro SD Card SPI slave model
 *
 * Translates SPI byte stream to/from QEMU SDState commands.
 * Wraps the built-in SD card model for block storage.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 * License: GPLv2+
 */

#pragma once

#include "hw/qdev-core.h"
#include "hw/sd/sd.h"

#define TYPE_TDECK_SD_SPI "tdeck-sd-spi"
#define TDECK_SD_SPI(obj) OBJECT_CHECK(TdeckSdSpiState, (obj), TYPE_TDECK_SD_SPI)

typedef enum {
    SD_SPI_IDLE,
    SD_SPI_RECEIVING_CMD,
    SD_SPI_RESPONDING,
    SD_SPI_READING_DATA,
    SD_SPI_WAIT_WRITE_TOKEN,
    SD_SPI_RECEIVING_WRITE,
    SD_SPI_BUSY,
} SdSpiProtoState;

typedef struct TdeckSdSpiState {
    DeviceState parent_obj;

    SDState *sd;                /* QEMU built-in SD card model (NULL if no drive) */
    bool inserted;              /* Card present (toggled by panel) */

    /* SPI protocol state machine */
    SdSpiProtoState state;

    uint8_t cmd_buf[6];        /* Command frame accumulator */
    uint8_t cmd_idx;

    uint8_t resp_buf[17];      /* Response buffer (R1 + up to 16 trailing bytes) */
    uint8_t resp_len;
    uint8_t resp_idx;

    uint8_t data_buf[514];     /* 512 data + 2 CRC */
    uint16_t data_len;
    uint16_t data_idx;

    bool app_cmd;              /* CMD55 prefix active */
    uint8_t busy_cycles;       /* Remaining busy (0x00) bytes after write */
} TdeckSdSpiState;

/*
 * Process one SPI byte exchange.
 * mosi: byte from host (firmware) to card
 * Returns: byte from card to host (MISO)
 */
uint8_t tdeck_sd_spi_transfer(TdeckSdSpiState *s, uint8_t mosi);

/*
 * Set card insertion state. When ejected, all transfers return 0xFF.
 * Resets protocol state machine on eject.
 */
void tdeck_sd_spi_set_inserted(TdeckSdSpiState *s, bool inserted);
```

- [ ] **Step 2: Commit**

```bash
git add include/hw/ssi/tdeck_sd_spi.h
git commit -m "feat(sd): add SD SPI slave model header with protocol state machine types"
```

---

## Task 2: SD SPI Protocol Model — Implementation

**Files:**
- Create: `hw/ssi/tdeck_sd_spi.c`
- Modify: `hw/ssi/meson.build`

This is the core of the SD emulation. The state machine translates between SPI byte exchanges and QEMU's SDState API (`sd_do_command`, `sd_read_byte`, `sd_write_byte`).

Reference: QEMU's `hw/sd/ssi-sd.c` implements a similar SPI-to-SD translation for the SSI bus. Our model follows the same protocol but calls SDState directly instead of going through SSI.

- [ ] **Step 1: Create the implementation file**

```c
/*
 * T-Deck Pro SD Card SPI slave model
 *
 * Translates the SPI byte stream from the GP-SPI controller into
 * SD commands via QEMU's built-in SDState model. Handles SPI-specific
 * framing: command accumulation, R1/R7 responses, data tokens (0xFE),
 * write data response (0x05), and busy signaling.
 *
 * The firmware sends standard SD SPI commands (CMD0, CMD8, ACMD41,
 * CMD17, CMD24, etc.) over SPI2 with GPIO48 as chip select.
 *
 * Copyright (c) 2026 T-Deck Pro Project
 * License: GPLv2+
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/ssi/tdeck_sd_spi.h"

/* SD SPI command indices */
#define SD_CMD0   0   /* GO_IDLE_STATE */
#define SD_CMD8   8   /* SEND_IF_COND */
#define SD_CMD9   9   /* SEND_CSD */
#define SD_CMD16  16  /* SET_BLOCKLEN */
#define SD_CMD17  17  /* READ_SINGLE_BLOCK */
#define SD_CMD24  24  /* WRITE_BLOCK */
#define SD_CMD55  55  /* APP_CMD */
#define SD_CMD58  58  /* READ_OCR */
#define SD_ACMD41 41  /* SD_SEND_OP_COND (after CMD55) */

static void tdeck_sd_spi_reset_state(TdeckSdSpiState *s)
{
    s->state = SD_SPI_IDLE;
    s->cmd_idx = 0;
    s->resp_len = 0;
    s->resp_idx = 0;
    s->data_len = 0;
    s->data_idx = 0;
    s->app_cmd = false;
    s->busy_cycles = 0;
}

void tdeck_sd_spi_set_inserted(TdeckSdSpiState *s, bool inserted)
{
    s->inserted = inserted;
    if (!inserted) {
        tdeck_sd_spi_reset_state(s);
    }
}

/*
 * Execute an accumulated 6-byte command frame.
 * Populates resp_buf with the R1 byte and any trailing response data.
 * Returns the next protocol state to transition to.
 */
static SdSpiProtoState tdeck_sd_spi_exec_cmd(TdeckSdSpiState *s)
{
    uint8_t cmd_idx = s->cmd_buf[0] & 0x3F;
    uint32_t arg = ((uint32_t)s->cmd_buf[1] << 24) |
                   ((uint32_t)s->cmd_buf[2] << 16) |
                   ((uint32_t)s->cmd_buf[3] << 8) |
                   (uint32_t)s->cmd_buf[4];

    /* Handle CMD55 (APP_CMD prefix) */
    if (cmd_idx == SD_CMD55) {
        SDRequest req = { .cmd = cmd_idx, .arg = arg };
        uint8_t response[16];
        sd_do_command(s->sd, &req, response);
        s->app_cmd = true;
        s->resp_buf[0] = 0x01;  /* R1: idle */
        s->resp_len = 1;
        s->resp_idx = 0;
        return SD_SPI_RESPONDING;
    }

    /* Build SDRequest */
    SDRequest req;
    if (s->app_cmd) {
        req.cmd = cmd_idx | 0x40;  /* ACMD flag for SDState */
        s->app_cmd = false;
    } else {
        req.cmd = cmd_idx;
    }
    req.arg = arg;
    req.crc = s->cmd_buf[5];

    /* Execute command */
    uint8_t response[16];
    int rlen = sd_do_command(s->sd, &req, response);

    /*
     * Build SPI response.
     * In SPI mode, R1 is always the first byte (bit 7=0).
     * SDState returns the response in SD-native format;
     * we need to translate to SPI format.
     */
    s->resp_idx = 0;

    switch (cmd_idx) {
    case SD_CMD0:
        /* R1: in-idle-state */
        s->resp_buf[0] = 0x01;
        s->resp_len = 1;
        return SD_SPI_RESPONDING;

    case SD_CMD8:
        /* R7: R1 + 4 bytes (echo back voltage + check pattern) */
        s->resp_buf[0] = 0x01;  /* R1: idle */
        s->resp_buf[1] = 0x00;
        s->resp_buf[2] = 0x00;
        s->resp_buf[3] = (arg >> 8) & 0xFF;  /* voltage accepted */
        s->resp_buf[4] = arg & 0xFF;          /* echo check pattern */
        s->resp_len = 5;
        return SD_SPI_RESPONDING;

    case SD_ACMD41:
        /* R1: 0x00 when ready, 0x01 when still initializing */
        /* SDState handles the init logic; check if card is ready */
        s->resp_buf[0] = sd_is_ready(s->sd) ? 0x00 : 0x01;
        s->resp_len = 1;
        return SD_SPI_RESPONDING;

    case SD_CMD58: {
        /* R3: R1 + 4 bytes OCR */
        s->resp_buf[0] = 0x00;  /* R1: ready */
        /* OCR: CCS=1 (SDHC), power-up complete, 3.3V */
        s->resp_buf[1] = 0xC0;  /* CCS=1, power-up=1 */
        s->resp_buf[2] = 0xFF;  /* voltage window */
        s->resp_buf[3] = 0x80;
        s->resp_buf[4] = 0x00;
        s->resp_len = 5;
        return SD_SPI_RESPONDING;
    }

    case SD_CMD9: {
        /* R1 + data block (16 bytes CSD + 2 CRC) */
        s->resp_buf[0] = 0x00;
        s->resp_len = 1;
        /* Data block will follow after response */
        s->data_buf[0] = 0xFE;  /* data start token */
        for (int i = 0; i < 16; i++) {
            s->data_buf[1 + i] = sd_read_byte(s->sd);
        }
        s->data_buf[17] = 0x00;  /* CRC dummy */
        s->data_buf[18] = 0x00;
        s->data_len = 19;  /* token + 16 CSD + 2 CRC */
        s->data_idx = 0;
        return SD_SPI_RESPONDING;  /* response first, then data */
    }

    case SD_CMD16:
        /* R1: success */
        s->resp_buf[0] = 0x00;
        s->resp_len = 1;
        return SD_SPI_RESPONDING;

    case SD_CMD17:
        /* R1 response, then data block follows */
        s->resp_buf[0] = 0x00;
        s->resp_len = 1;
        /* Pre-fill data buffer from SDState */
        s->data_buf[0] = 0xFE;  /* data start token */
        for (int i = 0; i < 512; i++) {
            s->data_buf[1 + i] = sd_read_byte(s->sd);
        }
        s->data_buf[513] = 0x00;  /* CRC dummy */
        s->data_buf[514] = 0x00;
        s->data_len = 515;  /* token + 512 data + 2 CRC */
        s->data_idx = 0;
        return SD_SPI_RESPONDING;  /* response first, then data */

    case SD_CMD24:
        /* R1 response, then wait for write data token */
        s->resp_buf[0] = 0x00;
        s->resp_len = 1;
        s->data_idx = 0;
        s->data_len = 514;  /* 512 data + 2 CRC to receive */
        return SD_SPI_RESPONDING;  /* response first, then wait for token */

    default:
        /* Generic R1 response */
        s->resp_buf[0] = (rlen > 0) ? response[0] : 0x00;
        s->resp_len = 1;
        return SD_SPI_RESPONDING;
    }
}

/*
 * Determine next state after response bytes are drained.
 * For commands with data phases, transition to the data state.
 */
static SdSpiProtoState tdeck_sd_spi_after_response(TdeckSdSpiState *s)
{
    uint8_t cmd_idx = s->cmd_buf[0] & 0x3F;

    switch (cmd_idx) {
    case SD_CMD9:
    case SD_CMD17:
        return SD_SPI_READING_DATA;
    case SD_CMD24:
        return SD_SPI_WAIT_WRITE_TOKEN;
    default:
        return SD_SPI_IDLE;
    }
}

uint8_t tdeck_sd_spi_transfer(TdeckSdSpiState *s, uint8_t mosi)
{
    /* No card or ejected: return 0xFF (bus idle) */
    if (!s->sd || !s->inserted) {
        return 0xFF;
    }

    switch (s->state) {
    case SD_SPI_IDLE:
        /* Check for command start byte: bit 7=0, bit 6=1 */
        if ((mosi & 0xC0) == 0x40) {
            s->cmd_buf[0] = mosi;
            s->cmd_idx = 1;
            s->state = SD_SPI_RECEIVING_CMD;
        }
        return 0xFF;

    case SD_SPI_RECEIVING_CMD:
        s->cmd_buf[s->cmd_idx++] = mosi;
        if (s->cmd_idx >= 6) {
            /* Full command received, execute it */
            SdSpiProtoState next = tdeck_sd_spi_exec_cmd(s);
            s->state = next;
        }
        return 0xFF;

    case SD_SPI_RESPONDING: {
        uint8_t val = s->resp_buf[s->resp_idx++];
        if (s->resp_idx >= s->resp_len) {
            s->state = tdeck_sd_spi_after_response(s);
        }
        return val;
    }

    case SD_SPI_READING_DATA: {
        if (s->data_idx < s->data_len) {
            return s->data_buf[s->data_idx++];
        }
        /* Data block complete */
        s->state = SD_SPI_IDLE;
        return 0xFF;
    }

    case SD_SPI_WAIT_WRITE_TOKEN:
        if (mosi == 0xFE) {
            s->data_idx = 0;
            s->state = SD_SPI_RECEIVING_WRITE;
        }
        return 0xFF;

    case SD_SPI_RECEIVING_WRITE:
        if (s->data_idx < s->data_len) {
            s->data_buf[s->data_idx++] = mosi;
        }
        if (s->data_idx >= s->data_len) {
            /* Write 512 data bytes to SDState (skip 2 CRC bytes) */
            for (int i = 0; i < 512; i++) {
                sd_write_byte(s->sd, s->data_buf[i]);
            }
            s->busy_cycles = 2;
            s->state = SD_SPI_BUSY;
            return 0x05;  /* Data response: accepted */
        }
        return 0xFF;

    case SD_SPI_BUSY:
        if (s->busy_cycles > 0) {
            s->busy_cycles--;
            return 0x00;  /* Busy signal */
        }
        s->state = SD_SPI_IDLE;
        return 0xFF;  /* Ready */

    default:
        s->state = SD_SPI_IDLE;
        return 0xFF;
    }
}

/* QOM boilerplate — minimal, no realize/reset needed (state set by machine init) */
static void tdeck_sd_spi_class_init(ObjectClass *klass, void *data)
{
    /* No special class setup needed */
}

static const TypeInfo tdeck_sd_spi_info = {
    .name = TYPE_TDECK_SD_SPI,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TdeckSdSpiState),
    .class_init = tdeck_sd_spi_class_init,
};

static void tdeck_sd_spi_register_types(void)
{
    type_register_static(&tdeck_sd_spi_info);
}

type_init(tdeck_sd_spi_register_types)
```

**Note on `sd_is_ready`:** This function may not exist in the QEMU fork. If not, check `sd->state` directly or use `sd_data_ready()`. The ACMD41 response needs to return R1=0x00 when the card has completed initialization. Verify the SDState API during implementation and adjust accordingly.

**Note on ACMD encoding:** QEMU's `sd_do_command` may use `req.cmd = 41` for ACMD41 after CMD55 sets the app_cmd flag internally on SDState. Check `hw/sd/sd.c` for how CMD55 + ACMD41 interacts. The `| 0x40` encoding is used by some QEMU versions to signal ACMDs. Verify and adjust.

- [ ] **Step 2: Add to build system**

In `hw/ssi/meson.build`, find the line:

```
system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c', 'esp32s3_gpspi.c'))
```

Change to:

```
system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('esp32s3_spi.c', 'esp32s3_gpspi.c', 'tdeck_sd_spi.c'))
```

- [ ] **Step 3: Build and verify**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3
./qbuild
```

Expected: compiles with no new warnings (only pre-existing ones).

- [ ] **Step 4: Commit**

```bash
git add hw/ssi/tdeck_sd_spi.c include/hw/ssi/tdeck_sd_spi.h hw/ssi/meson.build
git commit -m "feat(sd): implement SD SPI protocol state machine wrapping QEMU SDState"
```

---

## Task 3: GP-SPI Routing — Add SD Card CS Dispatch and RX DMA

**Files:**
- Modify: `hw/ssi/esp32s3_gpspi.c`
- Modify: `include/hw/ssi/esp32s3_gpspi.h`

The GP-SPI currently only routes TX DMA data to the EPD when GPIO34 is LOW. This task adds:
1. GPIO48 check for SD card CS
2. Per-byte SPI exchange via `tdeck_sd_spi_transfer()`
3. RX DMA writeback via `esp_gdma_write_channel()` so firmware reads MISO data

- [ ] **Step 1: Add SD SPI pointer to GP-SPI state**

In `include/hw/ssi/esp32s3_gpspi.h`, add after the `epd` field:

```c
    TdeckSdSpiState *sd_spi;           /* SD card SPI slave (optional) */
```

Also add the forward declaration near the top (after the existing TdeckUc8253State forward decl):

```c
typedef struct TdeckSdSpiState TdeckSdSpiState;
```

- [ ] **Step 2: Add SD routing in the SPI transfer function**

In `hw/ssi/esp32s3_gpspi.c`, include the SD header at the top:

```c
#include "hw/ssi/tdeck_sd_spi.h"
```

In the transfer execution code (triggered when USR bit is written), find the existing EPD CS check block. After it (but still inside the `if (s->gdma && s->gpio)` block), add the SD card routing:

```c
        /* SD card: GPIO48 LOW = selected */
        bool sd_cs = !(s->gpio->out[1] & (1 << 16));  /* GPIO48 = 32+16 */
        if (sd_cs && s->sd_spi) {
            uint32_t ms_dlen = s->regs[SPI_MS_DLEN_REG / 4];
            uint32_t byte_count = (ms_dlen + 1) / 8;

            uint32_t tx_chan, rx_chan;
            bool has_tx = esp_gdma_get_channel_periph(s->gdma,
                (GdmaPeripheral)s->gdma_periph_id, ESP_GDMA_OUT_IDX, &tx_chan);
            bool has_rx = esp_gdma_get_channel_periph(s->gdma,
                (GdmaPeripheral)s->gdma_periph_id, ESP_GDMA_IN_IDX, &rx_chan);

            if (has_tx) {
                uint8_t *tx_buf = g_malloc0(byte_count);
                uint8_t *rx_buf = g_malloc0(byte_count);

                esp_gdma_read_channel_data(s->gdma, tx_chan, tx_buf, byte_count);

                /* Full-duplex SPI exchange: each TX byte produces an RX byte */
                for (uint32_t i = 0; i < byte_count; i++) {
                    rx_buf[i] = tdeck_sd_spi_transfer(s->sd_spi, tx_buf[i]);
                }

                /* Write MISO data back via RX DMA channel */
                if (has_rx) {
                    esp_gdma_write_channel(s->gdma, rx_chan, rx_buf, byte_count);
                }

                g_free(tx_buf);
                g_free(rx_buf);
            }
        }
```

- [ ] **Step 3: Build and verify**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3
./qbuild
```

- [ ] **Step 4: Commit**

```bash
git add hw/ssi/esp32s3_gpspi.c include/hw/ssi/esp32s3_gpspi.h
git commit -m "feat(gpspi): route SPI transfers to SD card when GPIO48 CS is asserted"
```

---

## Task 4: Machine Init — Create SD Device and Wire Everything

**Files:**
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `include/hw/xtensa/esp32s3.h` (if SoC state struct is here, otherwise it may be in esp32s3.c)

- [ ] **Step 1: Add SD SPI state to SoC struct**

Find the `Esp32s3SocState` struct. Add after the `epd` field:

```c
    TdeckSdSpiState sd_spi;              /* SD card SPI slave */
```

Add the include at the top of the header:

```c
#include "hw/ssi/tdeck_sd_spi.h"
```

- [ ] **Step 2: Create and wire SD device in machine init**

In `esp32s3.c`, find the section after the EPD is wired to GP-SPI (around line 995 where `ss->gpspi[0].epd = &ss->epd` is set). Add:

```c
    /* SD Card SPI slave — backed by -drive if=sd */
    {
        DriveInfo *sd_dinfo = drive_get(IF_SD, 0, 0);
        if (sd_dinfo) {
            BlockBackend *sd_blk = blk_by_legacy_dinfo(sd_dinfo);
            ss->sd_spi.sd = sd_init(sd_blk, true);  /* true = SPI mode */
            ss->sd_spi.inserted = true;
        }
        /* Wire SD SPI into GP-SPI for CS routing */
        ss->gpspi[0].sd_spi = &ss->sd_spi;
        /* Wire into EPD panel for insert/eject button */
        ss->epd.sd_spi = &ss->sd_spi;
    }
```

**Important:** The existing `esp32s3_machine_init_sd()` function (around line 337) creates an SD card on the SDMMC controller using the same `drive_get(IF_SD, 0, 0)`. Since the firmware uses SPI mode (not SDMMC), either:
- Remove or disable the existing SDMMC SD card creation, OR
- Use a different drive interface (e.g., `IF_NONE` with an explicit `-drive id=sd0` and match by ID)

The simplest approach: remove the SDMMC SD card creation since the firmware never uses the SDMMC controller. Comment out or delete the `esp32s3_machine_init_sd()` call and its body. The SPI-mode SD card replaces it.

- [ ] **Step 3: Build and verify**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3
./qbuild
```

- [ ] **Step 4: Commit**

```bash
git add hw/xtensa/esp32s3.c include/hw/xtensa/esp32s3.h
git commit -m "feat(machine): create SPI-mode SD card device and wire into GP-SPI"
```

---

## Task 5: Control Panel — SD Card Insert/Eject Button

**Files:**
- Modify: `hw/display/tdeck_uc8253.c`
- Modify: `include/hw/display/tdeck_uc8253.h`

- [ ] **Step 1: Add SD SPI pointer and panel state to EPD struct**

In `include/hw/display/tdeck_uc8253.h`, add the forward declaration:

```c
typedef struct TdeckSdSpiState TdeckSdSpiState;
```

Add to `TdeckUc8253State` after the existing `bq25896` field:

```c
    TdeckSdSpiState *sd_spi;
```

Add to the panel state fields (after `panel_signal`):

```c
    int panel_sd_inserted;    /* 0=ejected, 1=inserted */
```

- [ ] **Step 2: Add SD section to panel renderer**

In `hw/display/tdeck_uc8253.c`, in `tdeck_panel_render()`, after the Signal button rendering (around line 219), add:

```c
    /* SD Card section */
    panel_puts(pixels, stride, px + 8, 200, "SD Card", fg, bg);
    if (s->sd_spi && s->sd_spi->sd) {
        const char *sd_btn = s->panel_sd_inserted ? "[Eject]" : "[Insert]";
        const char *sd_status = s->panel_sd_inserted ? "Inserted" : "Ejected";
        panel_puts(pixels, stride, px + 8, 220, sd_btn, fg, btn_bg);
        panel_puts(pixels, stride, px + 72, 220, sd_status, fg, bg);
    } else {
        panel_puts(pixels, stride, px + 8, 220, "No image", fg, bg);
    }
```

- [ ] **Step 3: Add SD click handler**

In `tdeck_panel_click()`, before the final `else { return; }`, add:

```c
    /* [Eject/Insert] SD card button: around (8-56, 220-236) */
    else if (px >= 8 && px <= 56 && py >= 220 && py <= 236) {
        if (s->sd_spi && s->sd_spi->sd) {
            s->panel_sd_inserted = !s->panel_sd_inserted;
            tdeck_sd_spi_set_inserted(s->sd_spi, s->panel_sd_inserted);
        }
    }
```

Also add the include at the top:

```c
#include "hw/ssi/tdeck_sd_spi.h"
```

- [ ] **Step 4: Initialize panel_sd_inserted in reset**

In `tdeck_uc8253_reset_hold()`, add:

```c
    s->panel_sd_inserted = (s->sd_spi && s->sd_spi->inserted) ? 1 : 0;
```

- [ ] **Step 5: Build and verify**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3
./qbuild
```

- [ ] **Step 6: Commit**

```bash
git add hw/display/tdeck_uc8253.c include/hw/display/tdeck_uc8253.h
git commit -m "feat(panel): add SD card insert/eject button to control panel"
```

---

## Task 6: Helper Script and qemu-run.sh Update

**Files:**
- Create: `scripts/qemu-sdcard.sh` (in the firmware repo, not QEMU fork)
- Modify: `scripts/qemu-run.sh`

- [ ] **Step 1: Create the SD card image helper script**

```bash
#!/bin/bash
# Create a FAT32 SD card image for QEMU.
# Requires: mtools (brew install mtools / apt install mtools)
#
# Usage: ./scripts/qemu-sdcard.sh [options] <output.img>
#   --size SIZE      Image size (default: 64M)
#   --file SRC:DST   Copy file into image (repeatable)
set -euo pipefail

SIZE="64M"
FILES=()
OUTPUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --size) SIZE="$2"; shift 2 ;;
        --file) FILES+=("$2"); shift 2 ;;
        -*) echo "Unknown option: $1"; exit 1 ;;
        *) OUTPUT="$1"; shift ;;
    esac
done

if [ -z "$OUTPUT" ]; then
    echo "Usage: $0 [--size SIZE] [--file SRC:DST]... <output.img>"
    exit 1
fi

echo "Creating ${SIZE} FAT32 image: ${OUTPUT}"
dd if=/dev/zero of="$OUTPUT" bs=1 count=0 seek="$SIZE" 2>/dev/null
mformat -F -i "$OUTPUT" ::

for entry in "${FILES[@]}"; do
    SRC="${entry%%:*}"
    DST="${entry#*:}"
    echo "  Copying $SRC -> $DST"
    mcopy -i "$OUTPUT" "$SRC" "::${DST}"
done

echo "Done: $OUTPUT"
```

- [ ] **Step 2: Update qemu-run.sh to support --sd flag**

In `scripts/qemu-run.sh`, add to the argument parsing loop:

```bash
        --sd)       SD_PATH="$2"; shift ;;
```

Add after the variable declarations at the top:

```bash
SD_PATH=""
SD_DRIVE=""
```

Add before the QEMU launch (after the display flags section):

```bash
if [ -n "$SD_PATH" ]; then
    if [[ "$SD_PATH" == dir:* ]]; then
        DIR_PATH="${SD_PATH#dir:}"
        SD_DRIVE="-drive file=fat:rw:${DIR_PATH},if=sd,format=raw"
        echo "  SD card: directory passthrough ($DIR_PATH)"
    else
        SD_DRIVE="-drive file=${SD_PATH},if=sd,format=raw"
        echo "  SD card: image ($SD_PATH)"
    fi
fi
```

Add `$SD_DRIVE` to the QEMU command line (after the serial lines, before `$GDB_FLAGS`).

- [ ] **Step 3: Make script executable and commit**

```bash
chmod +x scripts/qemu-sdcard.sh
git add scripts/qemu-sdcard.sh scripts/qemu-run.sh
git commit -m "feat(scripts): add SD card image helper and --sd flag to qemu-run.sh"
```

---

## Task 7: E2E Test Updates

**Files:**
- Modify: `scripts/qemu-e2e.sh`

- [ ] **Step 1: Add SD card image creation and test assertions**

In `scripts/qemu-e2e.sh`, add after the flash image generation (before QEMU launch):

```bash
echo "Creating temporary SD card image..."
SD_IMG="/tmp/tdeck-qemu-e2e-sd.img"
dd if=/dev/zero of="$SD_IMG" bs=1 count=0 seek=64M 2>/dev/null
mformat -F -i "$SD_IMG" :: 2>/dev/null || {
    echo "SKIP: mtools not installed, skipping SD card tests"
    SD_IMG=""
}

SD_DRIVE=""
if [ -n "$SD_IMG" ]; then
    SD_DRIVE="-drive file=$SD_IMG,if=sd,format=raw"
fi
```

Add `$SD_DRIVE` to the QEMU command line.

Add new test section after the Touch Controller section:

```bash
if [ -n "$SD_IMG" ]; then
    echo ""
    echo "--- SD Card ---"
    check "SD card available" "SD_AVAILABLE"
    check_absent "No SD init failure" "SD.*failed"
fi
```

Add cleanup at the end:

```bash
rm -f "$SD_IMG" /tmp/e2e_flash.bin
```

- [ ] **Step 2: Commit**

```bash
git add scripts/qemu-e2e.sh
git commit -m "test(e2e): add SD card initialization checks to QEMU E2E tests"
```

---

## Task 8: Integration Test — Full E2E Run

**Files:** None (verification only)

- [ ] **Step 1: Create a test SD card image**

```bash
cd /Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust
./scripts/qemu-sdcard.sh --size 64M sdcard.img
```

- [ ] **Step 2: Run firmware in QEMU with SD card**

```bash
./scripts/qemu-run.sh --headless --sd sdcard.img
```

Let it run for 20-30 seconds, then kill. Check defmt output for:
- SD card initialization messages (CMD0, ACMD41 completion)
- `SD_AVAILABLE` set to true
- No SD-related errors or timeouts

- [ ] **Step 3: Run the E2E test suite**

```bash
./scripts/qemu-e2e.sh
```

Expected: all existing tests pass + new SD card tests pass.

- [ ] **Step 4: Test directory passthrough**

```bash
mkdir -p /tmp/tdeck-sd-test
echo "test_ssid" > /tmp/tdeck-sd-test/wifi.cfg
./scripts/qemu-run.sh --headless --sd dir:/tmp/tdeck-sd-test
```

Verify firmware can read the wifi.cfg file.

---

## Summary

| Task | Files | Description |
|------|-------|-------------|
| 1 | `tdeck_sd_spi.h` | SD SPI model header with types and API |
| 2 | `tdeck_sd_spi.c`, `meson.build` | Protocol state machine implementation |
| 3 | `esp32s3_gpspi.c/.h` | GPIO48 CS routing + RX DMA writeback |
| 4 | `esp32s3.c/.h` | Create SD device, wire into GP-SPI and panel |
| 5 | `tdeck_uc8253.c/.h` | Control panel insert/eject button |
| 6 | `qemu-sdcard.sh`, `qemu-run.sh` | Helper scripts |
| 7 | `qemu-e2e.sh` | E2E test assertions |
| 8 | (none) | Integration verification |

Tasks 1-5 are in the QEMU fork. Tasks 6-8 are in the firmware repo. Tasks 1-2 have no dependencies. Task 3 depends on 1. Task 4 depends on 1+3. Task 5 depends on 1+4. Tasks 6-7 are independent of QEMU tasks. Task 8 depends on all others.
