# ESP32-S3 Peripheral Contract Follow-Ons Plan

## Goal
Close the residual board-path contract gaps in the already-modeled peripheral set without broadening them into unjustified silicon-complete rewrites.

## Current Phase
Queued on 2026-04-19 under `docs/plans/new/`.

## Scope
This plan covers:

- GP-SPI routed-signal ownership, default board-pin assumptions, and staged timing residuals
- residual USB Serial/JTAG, I2C, UART, and SHA contract gaps such as uncommon modes, DMA error paths, and multi-CPU interrupt routing
- the remaining synthetic boundaries in eFuse, PMS, and RNG where a stronger source-backed contract is feasible without claiming physical hardware behavior

This phase does not claim complete silicon timing or analog behavior for these blocks.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/ssi/esp32s3_gpspi.c`
- `hw/gpio/esp32s3_gpio.c`
- `hw/char/esp32s3_usb_serial_jtag.c`
- `hw/i2c/esp32s3_i2c.c`
- `hw/char/esp32s3_uart.c`
- `hw/misc/esp32s3_sha.c`
- `hw/nvram/esp32s3_efuse.c`
- `hw/misc/esp32s3_pms.c`
- `hw/misc/esp32s3_rng.c`
- `tests/qtest/esp32s3-test.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc`
- adjacent firmware board-path usage in `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/src`

## File Map
- Modify: `hw/ssi/esp32s3_gpspi.c`
- Modify: `hw/gpio/esp32s3_gpio.c`
- Modify: `hw/char/esp32s3_usb_serial_jtag.c`
- Modify: `hw/i2c/esp32s3_i2c.c`
- Modify: `hw/char/esp32s3_uart.c`
- Modify: `hw/misc/esp32s3_sha.c`
- Modify: `hw/nvram/esp32s3_efuse.c`
- Modify: `hw/misc/esp32s3_pms.c`
- Modify: `hw/misc/esp32s3_rng.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/gpspi`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/i2c`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/uart`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/sha`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/pms`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/rng`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/efuse`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`

## Tasks
- [ ] Audit each residual peripheral gap against the active firmware path so the next slices are driven by real usage rather than speculative completeness.
- [ ] Remove hard-wired GP-SPI board assumptions where the GPIO routing and firmware evidence justify a more explicit contract.
- [ ] Tighten the remaining uncommon-mode or interrupt-routing surfaces in USB Serial/JTAG, I2C, UART, and SHA as they become guest-proven.
- [ ] Promote any stronger source-backed eFuse, PMS, or RNG contract that improves correctness without pretending to model factory provisioning, real security policy, or physical entropy behavior.
- [ ] Keep `ESP32S3_EMULATION_GAPS.md` aligned with the residual contract after each landed slice.

## Exit Criteria
- The already-modeled peripherals have fewer implicit board assumptions and fewer unpinned residual behaviors.
- The remaining synthetic boundaries in eFuse, PMS, and RNG are explicitly justified rather than accidental.
