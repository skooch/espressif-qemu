# QEMU SD Card SPI Emulation — Design Spec

## Goal

Add SD card emulation to the QEMU T-Deck Pro emulator so the firmware's SPI-based SD driver initializes, reads, and writes a FAT filesystem backed by a host file or directory. Include a control panel button for hot insert/eject.

## Background

The T-Deck Pro firmware accesses the SD card via SPI mode on SPI2 (CS=GPIO48), sharing the bus with the EPD (CS=GPIO34) and LoRa (CS=GPIO3). The firmware uses `embedded-sdmmc` for FAT filesystem access. SD card storage is used for WiFi credential migration, file I/O, and notes.

QEMU already has a mature `SDState` model that handles all SD commands, CRC, OCR, CSD, and block storage. Rather than reimplement the SD protocol, the new model wraps `SDState` and translates between the SPI byte stream and SD command/response semantics.

## Architecture

```
GP-SPI (esp32s3_gpspi.c)
  |
  |-- GPIO34 LOW? --> EPD (tdeck_uc8253.c)      [existing]
  |-- GPIO48 LOW? --> SD SPI (tdeck_sd_spi.c)    [new]
  |-- else         --> discard (no slave)

tdeck_sd_spi.c
  |
  |-- SPI byte framing (command accumulation, response tokens, data blocks)
  |
  v
SDState (hw/sd/sd.c)   <-- QEMU built-in SD card model
  |
  v
BlockBackend            <-- host file or vvfat directory
```

## SD SPI Protocol Model

### Device: `tdeck-sd-spi`

New file `hw/ssi/tdeck_sd_spi.c` with header `include/hw/ssi/tdeck_sd_spi.h`.

### State Struct

```c
typedef struct TdeckSdSpiState {
    DeviceState parent_obj;
    SDState *sd;              /* QEMU built-in SD card model */
    bool inserted;            /* Card present (toggled by panel button) */

    /* SPI protocol state machine */
    enum {
        SD_SPI_IDLE,          /* Returning 0xFF, waiting for cmd start */
        SD_SPI_RECEIVING_CMD, /* Accumulating 6-byte command frame */
        SD_SPI_RESPONDING,    /* Draining response bytes */
        SD_SPI_READING_DATA,  /* Sending 0xFE + 512 data + 2 CRC */
        SD_SPI_WAIT_WRITE_TOKEN,  /* Waiting for 0xFE from host */
        SD_SPI_RECEIVING_WRITE,   /* Accumulating 512 + 2 bytes */
        SD_SPI_BUSY,          /* Returning 0x00 after write */
    } state;

    uint8_t cmd_buf[6];      /* Command accumulator */
    uint8_t cmd_idx;          /* Bytes received into cmd_buf */
    uint8_t resp_buf[17];     /* Response buffer (R1 + up to 16 bytes) */
    uint8_t resp_len;         /* Total response bytes */
    uint8_t resp_idx;         /* Next response byte to send */
    uint8_t data_buf[514];    /* 512 data + 2 CRC */
    uint16_t data_len;        /* Total data bytes to send/receive */
    uint16_t data_idx;        /* Current position in data_buf */
    bool app_cmd;             /* CMD55 flag: next command is ACMD */
    uint8_t busy_cycles;      /* Remaining 0x00 bytes to send after write */
} TdeckSdSpiState;
```

### Public API

```c
/* Process one SPI byte exchange. Returns MISO byte. */
uint8_t tdeck_sd_spi_transfer(TdeckSdSpiState *s, uint8_t mosi);

/* Hot insert/eject for control panel */
void tdeck_sd_spi_set_inserted(TdeckSdSpiState *s, bool inserted);
```

### State Machine

**IDLE:**
- If `!inserted`, return 0xFF always.
- If received byte has bit 7=0 and bit 6=1, it is a command start byte. Store in `cmd_buf[0]`, transition to `RECEIVING_CMD`.
- Otherwise return 0xFF.

**RECEIVING_CMD:**
- Accumulate bytes into `cmd_buf[1..5]`.
- On 6th byte: decode command index (`cmd_buf[0] & 0x3F`), argument (`cmd_buf[1..4]` big-endian).
- If `app_cmd` is set, treat as ACMD, clear flag.
- If CMD55, set `app_cmd = true`, generate R1 response.
- Forward command to `SDState` via `sd_do_command()`.
- Build response in `resp_buf`: R1 byte first, then any command-specific trailing bytes (R7 for CMD8, OCR for CMD58, CSD for CMD9).
- For CMD17: after response, transition to `READING_DATA` (fill `data_buf` from `sd_read_byte()`).
- For CMD24: after response, transition to `WAIT_WRITE_TOKEN`.
- For other commands: transition to `RESPONDING`.

**RESPONDING:**
- Return `resp_buf[resp_idx++]`.
- When `resp_idx == resp_len`, transition to `IDLE`.

**READING_DATA:**
- First send 0xFE (data start token).
- Then send 512 data bytes from `data_buf`.
- Then send 2 dummy CRC bytes (0x00, 0x00).
- Transition to `IDLE`.

**WAIT_WRITE_TOKEN:**
- Return 0xFF.
- If received byte is 0xFE, transition to `RECEIVING_WRITE`, reset `data_idx = 0`.

**RECEIVING_WRITE:**
- Store incoming bytes in `data_buf`.
- After 514 bytes (512 data + 2 CRC): write data to `SDState` via `sd_write_byte()`, send data response token (0x05), transition to `BUSY`.

**BUSY:**
- Return 0x00 for `busy_cycles` (e.g., 2 cycles).
- Then return 0xFF and transition to `IDLE`.

### SDState Integration

Use QEMU's existing SD card infrastructure. The exact API (`sd_do_command`, `sd_read_byte`, `sd_write_byte`, `sd_init`) must be verified against this fork's `hw/sd/sd.c` during planning — function signatures may differ from upstream QEMU.

```c
/* In machine init (esp32s3.c): */
DriveInfo *dinfo = drive_get(IF_SD, 0, 0);
if (dinfo) {
    blk = blk_by_legacy_dinfo(dinfo);
    sd = sd_init(blk, true);   /* true = SPI mode */
    sd_set_cb(sd, NULL, NULL); /* No card-detect/write-protect GPIOs */
    sd_spi_state->sd = sd;
    sd_spi_state->inserted = true;
}
```

The `-drive` flag on the QEMU command line provides the backing:
- Raw image: `-drive file=sdcard.img,if=sd,format=raw`
- Directory passthrough: `-drive file=fat:rw:/path/to/dir,if=sd,format=raw`

When no `-drive` is provided, the SD model is created with `sd = NULL` and `inserted = false`. The panel still shows the SD section but the button does nothing.

## GP-SPI Routing Changes

In `esp32s3_gpspi.c`, the transfer execution function currently checks GPIO34 (EPD CS). Add GPIO48 check:

```c
/* Check which slave is selected */
bool epd_cs = !gpio_get_output(s->gpio, 34);
bool sd_cs  = !gpio_get_output(s->gpio, 48);

if (epd_cs && s->epd) {
    /* existing EPD dispatch */
    tdeck_uc8253_spi_receive(s->epd, tx_data, len, dc_level);
} else if (sd_cs && s->sd_spi) {
    /* SD card: exchange bytes one at a time */
    for (uint32_t i = 0; i < len; i++) {
        rx_data[i] = tdeck_sd_spi_transfer(s->sd_spi, tx_data[i]);
    }
    /* Write received data back to DMA RX buffer */
}
```

Key difference from EPD: the SD card returns data (MISO). The GP-SPI must write received bytes back into the DMA RX channel so the firmware can read responses. EPD is write-only (no MISO data used), so this path doesn't exist yet. The GP-SPI will need to call `esp_gdma_write_channel_data()` for the RX direction when an SD transfer occurs.

## MISO / DMA RX Path

The GP-SPI currently only handles DMA TX (reading data from firmware memory to send). For SD card, it must also handle DMA RX (writing received data back to firmware memory).

The existing `esp_gdma_read_channel_data()` reads from the TX descriptor chain. A corresponding `esp_gdma_write_channel_data()` (or equivalent) must write to the RX descriptor chain. Check if this already exists in the GDMA model; if not, implement it following the same descriptor-walking pattern as the TX path but writing instead of reading.

The RX path must:
1. Walk the RX DMA descriptor chain for the SPI2 channel
2. Write the MISO bytes from `tdeck_sd_spi_transfer()` into the RX buffers
3. Set the `suc_eof` flag on the last descriptor
4. Trigger the RX DMA completion interrupt

## Control Panel: Insert/Eject Button

In `hw/display/tdeck_uc8253.c`, add an SD card section to the control panel below the Cellular section:

**Panel rendering:**
```
SD Card
[Eject]  Inserted     (or [Insert]  Ejected)
```

**Click handler:**
- Toggle `sd_spi->inserted` via `tdeck_sd_spi_set_inserted()`.
- When ejecting: the SD SPI model resets its state machine to `IDLE` and returns 0xFF for all transfers. The firmware's next SD operation will fail and set `SD_AVAILABLE = false`.
- When inserting: the SD SPI model becomes responsive again. The firmware must re-initialize the card (which happens when the SD task retries or is restarted).

**Wiring:**
- `TdeckUc8253State` gets a `TdeckSdSpiState *sd_spi` pointer, set during machine init.
- Panel state: `int panel_sd_inserted` (tracks display state, synced from `sd_spi->inserted`).

## Machine Init Wiring (esp32s3.c)

After GP-SPI and EPD are created:

```c
/* SD Card SPI slave */
DriveInfo *sd_drive = drive_get(IF_SD, 0, 0);
if (sd_drive) {
    BlockBackend *sd_blk = blk_by_legacy_dinfo(sd_drive);
    /* Create SD state in SPI mode */
    ss->sd_spi.sd = sd_init(sd_blk, false);
    ss->sd_spi.inserted = true;
} else {
    ss->sd_spi.sd = NULL;
    ss->sd_spi.inserted = false;
}
/* Wire SD SPI into GP-SPI and EPD panel */
ss->gpspi[0].sd_spi = &ss->sd_spi;
ss->epd.sd_spi = &ss->sd_spi;
```

The `TdeckSdSpiState` can be embedded directly in `Esp32s3SocState` (like EPD) rather than dynamically allocated, since there is exactly one SD card.

## Helper Script: `scripts/qemu-sdcard.sh`

Creates a FAT32 image for use with QEMU:

```bash
Usage: ./scripts/qemu-sdcard.sh [options] <output.img>
  --size SIZE    Image size (default: 64M)
  --file SRC:DST Copy host file SRC to image path DST
  --dir SRC:DST  Copy host directory SRC to image path DST

Example:
  ./scripts/qemu-sdcard.sh --size 64M --file wifi.cfg:/wifi.cfg sdcard.img
```

Implementation: uses `dd` to create image, `mkfs.fat -F 32` to format, `mtools` (`mcopy`) to copy files. Falls back to mount+cp on Linux if mtools is unavailable.

## qemu-run.sh Update

Add `--sd` flag:

```bash
--sd PATH   Mount SD card image or directory
            For images: --sd sdcard.img
            For directories: --sd dir:/path/to/dir
```

Translates to:
- Image: `-drive file=sdcard.img,if=sd,format=raw`
- Directory: `-drive file=fat:rw:/path/to/dir,if=sd,format=raw`

## E2E Test Updates

Extend `scripts/qemu-e2e.sh`:

1. Before QEMU launch: create temporary 64MB FAT32 image with a test `wifi.cfg`
2. Pass `--sd` flag to QEMU
3. Add assertions:
   - `check "SD card initialized" "SD_AVAILABLE"`
   - `check "SD init completes" "SD card init"` (or whatever the firmware logs)
   - `check_absent "No SD timeout" "SD.*timeout"`
4. Clean up temp image after test

## Acceptance Criteria

- Firmware SD task initializes successfully (CMD0 → CMD8 → ACMD41 → CMD58 → CMD9)
- `SD_AVAILABLE` flag set to true in defmt output
- WiFi credential file readable from SD image
- Write operations persist to host image file
- Directory passthrough (`fat:rw:`) works for development iteration
- Panel insert/eject button toggles card presence
- Ejecting causes firmware SD operations to fail gracefully
- Re-inserting allows firmware to re-initialize the card
- E2E tests pass with SD card checks
- No regressions in EPD or other SPI peripherals

## Out of Scope

- Multi-block read/write (CMD18/CMD25) — firmware uses single-block only
- SD card write protection
- Card-detect GPIO (hardware doesn't have one)
- SDIO mode (firmware uses SPI mode exclusively)
- LoRa SPI slave emulation (GPIO3 CS — separate future project)
