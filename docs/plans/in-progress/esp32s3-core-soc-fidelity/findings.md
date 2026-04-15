# ESP32-S3 Core SoC Fidelity Findings

## 2026-04-16 Generic MMIO Firmware Trace

Trace command used a copied flash image from `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/flash_image.bin`, the worktree `build/qemu-system-xtensa`, watchdog disabled, `-nic user,model=open_eth`, and `-trace 'esp32s3_unimplemented_io_*'`. A 15-second timeout is intentional for bounded collection.

Initial trace before SPI0 modeling:

- 289 trace log lines total.
- Dominant active catch-all region: `DR_REG_SPI0_BASE` / SPI_MEM at `0x60003000`.
- Highest hit: `0x60003054` (`SPI_MEM_FSM_REG(0)`) with 86 reads.
- Other SPI0 active offsets: `0x08`, `0x10`, `0x14`, `0x18`, `0x1c`, `0x20`, `0x24`, `0x28`, `0x34`, `0x3c`, `0x40`, `0x48`, `0x4c`, `0x50`, `0xdc`, `0xe0`, `0xe8`, and `0xec`.
- Evidence available: ESP-IDF `spi_mem_reg.h`, ESP-IDF bootloader flash configuration for ESP32-S3, and existing QEMU SPI_MEM model.
- Classification: boot-critical, source-backed enough for an explicit register-surface model.

Trace after mapping explicit SPI0 and extending active SPI_MEM register storage:

- 80 trace log lines total.
- No remaining `0x60003000` SPI0 hits.
- Remaining active catch-all addresses at this point:
- `0x60040000`, `0x60040004`, `0x60040018`, `0x60040028`, `0x60040038`, `0x6004003c`, `0x60040070`: `DR_REG_APB_SARADC_BASE`, source headers present, analog ADC behavior not modeled.
- `0x60008810`, `0x60008834`, `0x6000883c`: `DR_REG_SENS_BASE`, source headers present, analog sensor behavior not modeled.
- `0x60019000`, `0x60019004`, `0x60019008`, `0x6001900c`, `0x600190a0`, `0x600190d0`: `DR_REG_LEDC_BASE`, source headers present, PWM peripheral out of current core-SoC scope.
- `0x600ce048`, `0x600ce04c`, `0x600ce05c`: `DR_REG_ASSIST_DEBUG_BASE`, source headers present, core-debug surface but not a confirmed boot/sleep dependency.
- `0x600050f0`, `0x60006090`, `0x6001ccd4`, `0x6001d054`: FE2, FE, NRX, and BB radio/internal windows. No public register header was found in the local ESP-IDF register tree; classify as out of current core scope and blocked for accuracy without radio-internal references.

Trace after mapping a narrow ASSIST_DEBUG shim:

- 75 trace log lines total.
- No remaining `0x600ce000` ASSIST_DEBUG hits.
- Remaining active catch-all addresses are APB_SARADC, SENS, LEDC, FE2, FE, NRX, and BB only.

Prioritized follow-up from this trace:

- `P1`: Add a narrow APB_SARADC/SENS compatibility model only if core boot/sleep code needs these analog-control readbacks; otherwise document as peripheral/analog out of scope.
- `P1`: ASSIST_DEBUG core-debug catch-all hits are resolved by a narrow shim for `RCD_PDEBUGENABLE`, `RCD_RECORDING`, and `RCD_PDEBUGPC`.
- `P2`: Leave LEDC for the peripheral pass unless firmware-visible PWM behavior becomes part of a board-path test.
- `Blocked`: Do not attempt FE/BB/NRX accuracy from current sources; require exact radio-internal references or a deliberately documented compatibility shim.
