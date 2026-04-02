# ESP32-S3 Accuracy Doc And Immediate Fidelity Program

## Overview

This document tracks the known fidelity gaps in the `esp32s3` machine model in this fork and the immediate implementation program for closing the highest-value ones first.

The current scope is intentionally limited to the previously identified high-impact fixes and easy wins:

- SPI1 transfer correctness
- GP-SPI completion semantics
- routed GPIO / IO_MUX plumbing used by the current board path
- removal of boot-critical dependence on the generic MMIO echo region
- tighter USB Serial/JTAG, I2C, UART, PMS, RNG, and SHA behavior

Deferred items such as deep clock/reset rework, cache/MMU timing realism, `open_eth` replacement, and broader Xtensa backend fidelity remain out of scope for this immediate program.

## Current Gaps

- Unimplemented MMIO is often papered over instead of modeled. There is a giant catch-all register window that stores writes and echoes them back on reads, and it forces the PLL-calibration-done bit high so polling loops keep moving. RMT and IO_MUX were also explicitly mapped as unimplemented devices.
- Pin muxing and the GPIO matrix are not really driving peripherals yet. The GPIO func-in/func-out registers are stored, while GP-SPI routes traffic using hard-coded board pins like GPIO34, GPIO35, GPIO48, and GPIO3 rather than through routed signal decisions.
- SPI2/SPI3 were intentionally a minimum-function stub. Transfers complete instantly, `USR` clears immediately, and `TRANS_DONE` is raised right away instead of following a more realistic controller state progression.
- Clock, reset, and sleep/power behavior are only partially modeled. The `SYSTEM` block handles a small subset of registers, RTC sleep/wake logic is tailored to current timer/GPIO/EXT1 paths, and reset still contains QEMU-specific shims.
- Several peripherals are placeholders or heavily simplified. PMS is a dummy register file, RNG just returns host randomness, USB Serial/JTAG was TX-only with RX unimplemented and FIFO always ready, I2C had completion/readback shortcuts, and SHA documented DMA/IRQ behavior as incomplete.
- External memory and cache behavior are functional rather than cycle-accurate. Cache/MMU operations complete immediately and cache state reports idle.
- Some blocks are substituted with generic IP rather than an S3-specific model, notably Ethernet through `open_eth`.
- SPI1 had an outright correctness bug in the flash transfer loop: the byte loop compared the payload value instead of the loop index, making the transfer path data-dependent.
- The generic Xtensa backend still has known accuracy gaps such as missing local memory exclusion behavior and unimplemented opcode paths.

## Prioritized Backlog

### High Impact

- `P0` Fix the SPI1 transfer-loop bug so TX/RX bounds use the loop index instead of the payload byte value.
- `P1` Replace GP-SPI hard-coded board pin reads with a routed-signal layer backed by GPIO routing state and a minimal IO_MUX model for the current board path.
- `P1` Replace boot-critical generic-MMIO behavior with explicit narrow models for the firmware-touched offsets currently relied on.
- `P1` Make GP-SPI completion semantics more faithful than an unconditional immediate `USR` clear plus `TRANS_DONE`.

### Easy Wins

- `P2` Add USB Serial/JTAG RX support, FIFO-ready/backpressure semantics, and interrupt updates.
- `P2` Tighten I2C completion semantics, DONE-bit handling, ACK error behavior, and explicit rejection of unsupported modes.
- `P2` Make UART pulse timing and baud calculation derive from active clock state instead of fixed 40 MHz assumptions.
- `P2` Replace PMS raw echo behavior with reset/default/read-as-zero-write-ignore behavior for the currently touched surface.
- `P2` Keep RNG host-backed, but restrict it to a small modeled data path instead of broad undefined behavior.
- `P2` Wire SHA completion into realistic interrupt assert/clear behavior for existing command paths.

### Deferred

- `P3` Deep clock/reset/sleep fidelity rework to eliminate QEMU-only identity/workaround behavior.
- `P3` Cache/MMU sequencing and timing realism beyond boot-critical paths.
- `P3` Replace `open_eth` with a more ESP32-S3-specific EMAC model if firmware needs it.
- `P3` Broader Xtensa architectural fidelity work once guest firmware depends on those features.

## Immediate Program

### Stage 0: Doc

- Keep this file at repo root as the fork-local source of truth for the ESP32-S3 fidelity program.

### Stage 1: Regression Harness

- Add ESP32-S3-focused qtests rather than depending on large guest-firmware integration tests.
- Cover the first wave with direct MMIO- and device-level tests for:
  - SPI1 transfer correctness
  - GP-SPI completion/IRQ behavior
  - GPIO status and routing register behavior
  - USB Serial/JTAG TX/RX/FIFO state
  - I2C completion and ACK error signaling
  - UART baud/pulse behavior under different clock configurations
  - SHA IRQ assert/clear behavior
  - boot-critical MMIO offsets moved out of the generic echo region

### Stage 2: High-Impact Correctness Fix

- Fix SPI1 TX/RX loop bounds.
- Add a regression that uses payload values both below and above the transfer length so the old byte-vs-index bug would fail deterministically.

### Stage 3: IO_MUX + GPIO Matrix Routing

- Add an explicit minimal IO_MUX device instead of leaving the block unimplemented.
- Add one routed-signal layer so GP-SPI consumes routed board-control signals instead of hard-coded GPIO literals.
- Keep this first slice board-path-complete rather than silicon-complete: support the routed signals used by the current EPD, SD, and LoRa path and leave unrelated matrix functionality for later work.
- Status:
  - Done: IO_MUX device, routed signal plumbing for EPD/SD/LoRa control pins, richer staged GP-SPI transfer semantics, and qtest coverage for default + non-default remap state.

### Stage 4: Reduce Generic-MMIO Dependence

- Remove boot-critical PLL/ANA progress behavior from the generic MMIO echo region.
- Keep the fallback region only for genuinely unmodeled addresses that are outside the immediately supported firmware path.
- Status:
  - Done: boot-critical ANA `PLL_DONE` path moved into explicit model.

### Stage 5: Easy Wins

- USB Serial/JTAG:
  - add RX delivery from the attached chardev
  - report FIFO space based on actual buffered state
  - raise and clear interrupt state for RX/TX completion paths
- I2C:
  - stop forcing DONE bits on reads
  - clear/set DONE only when command state actually changes
  - preserve deferred completion IRQ delivery
  - keep unsupported slave/APB-nonfifo modes explicit
- UART:
  - compute baud and pulse timing from the current clock configuration
  - keep autobaud deterministic but clock-derived
- PMS:
  - stop treating the whole block as a raw echo register file
  - return reset/default or RAZ/WI behavior for the immediate surface
- RNG:
  - keep host entropy as the source
  - keep the visible register surface intentionally narrow
- SHA:
  - assert interrupt state when operations complete
  - make clear/enable semantics match the existing synchronous completion path
- Status:
  - Done: USB Serial/JTAG RX/FIFO-ready path, I2C DONE semantics, clock-derived UART timing, narrow RNG/PMS behaviors, SHA completion IRQ handling, and GP-SPI completion staging.

## Deferred Follow-Ups

- Expand routed-pin fidelity beyond the current board path.
- Model GP-SPI transfer timing, busy windows, and DMA completion with more realistic sequencing.
- Replace additional generic-MMIO dependencies as firmware begins to touch them.
- Revisit clock tree, cache/MMU, EMAC, and Xtensa backend fidelity once higher-priority peripheral behavior is stable.

## Source References

- `hw/xtensa/esp32s3.c`
- `hw/gpio/esp32s3_gpio.c`
- `hw/ssi/esp32s3_gpspi.c`
- `hw/ssi/esp32s3_spi.c`
- `hw/misc/esp32c3_jtag.c`
- `hw/i2c/esp32_i2c.c`
- `hw/char/esp32_uart.c`
- `hw/misc/esp32s3_pms.c`
- `hw/misc/esp32s3_rng.c`
- `hw/misc/esp_sha.c`
- `hw/xtensa/esp32s3_clk.c`
- `hw/misc/esp32s3_cache.c`
- `target/xtensa/op_helper.c`
- `target/xtensa/translate.c`
