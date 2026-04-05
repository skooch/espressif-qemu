# ESP32-S3 Accuracy Doc And Immediate Fidelity Program

## Overview

This document tracks the known fidelity gaps in the `esp32s3` machine model in this fork and the immediate implementation program for closing the highest-value ones first.

The immediate peripheral and deferred-foundation programs described here are now historical records. Active follow-on prioritization for the T-Deck Pro target lives in `docs/plans/new/esp32s3-tdeck-pro-fidelity/plan.md`.

The current scope is intentionally limited to the previously identified high-impact fixes and easy wins:

- SPI1 transfer correctness
- GP-SPI completion semantics
- routed GPIO / IO_MUX plumbing used by the current board path
- removal of boot-critical dependence on the generic MMIO echo region
- tighter USB Serial/JTAG, I2C, UART, PMS, RNG, and SHA behavior

Deferred items such as deep clock/reset rework, cache/MMU timing realism beyond the current packet/link contract, a full ESP32-S3-specific EMAC replacement, and broader Xtensa backend fidelity remain out of scope for this immediate program.

## Current State Snapshot

As of 2026-04-03, the target is materially stronger than a "boots-only" model, but it is still not a high-fidelity general-purpose ESP32-S3 hardware surrogate.

- The immediate stage-1 peripheral correctness program is complete and the focused `esp32s3` qtest suite is green.
- The current board path is in much better shape than before: SPI1, GP-SPI, routed GPIO/IO_MUX, USB Serial/JTAG, I2C, UART, PMS, RNG, and SHA now have direct regression coverage for the currently exercised surface.
- The recent EPD black-screen regression was traced to incorrect GDMA descriptor-address reconstruction. The runtime path now uses ESP32-S3 DMA RAM addressing semantics, the simulator renders again, and the qtests were aligned to the corrected hardware contract.
- The target is best understood as "board-path complete for the active firmware path, selectively modeled elsewhere". Many subsystems still behave functionally rather than faithfully.

### Current Fidelity / Risk Table

| Subsystem | Current state | Gap vs real hardware | Likely real-usage risk |
| --- | --- | --- | --- |
| Boot and active board path | Good enough for the current firmware path and focused regressions | Still relies on selective modeling rather than full-chip behavior | Medium |
| GP-SPI + GDMA | Good enough for current EPD, SD, LoRa, and qtest paths | Timing, busy windows, and broader DMA sequencing are still simplified | Medium |
| GPIO matrix + IO_MUX | Board-path complete for the routed signals in active use | Not a full silicon-complete routing model | Medium |
| Generic MMIO surface | Narrower than before, but still present | Unknown registers can still appear to work via stored readback | High |
| Clock / reset / sleep | Partially modeled | Deep sleep, wake, reset-domain, and wider clock-tree behavior remain incomplete | High |
| Cache / MMU / external memory | Functional and boot-capable | Operations complete too eagerly and state reporting is too optimistic | High |
| USB Serial/JTAG, I2C, UART, SHA | Board-path complete with RX, completion semantics, clock-derived timing, and IRQ coverage | DMA error paths, uncommon timing modes, and multi-CPU interrupt routing remain incomplete | Medium |
| PMS + RNG | Intentionally narrow modeled behavior | Useful for current firmware, not a full device-faithful implementation | Low to Medium |
| Ethernet | Generic `open_eth` stand-in with direct link/MII and descriptor loopback regression coverage | Still not an ESP32-S3-specific EMAC model or full PHY implementation | Low for the current workload, Medium to High if future firmware depends on deeper EMAC details |
| RMT | Unimplemented | Entire block still absent | High if firmware depends on it |
| Xtensa backend | Sufficient for current guest path | Architectural edge cases and local-memory exclusion remain incomplete | High for broader firmware coverage |

## Current Gaps

- Unimplemented MMIO is often papered over instead of modeled. There is a giant catch-all register window that stores writes and echoes them back on reads, and it forces the PLL-calibration-done bit high so polling loops keep moving. RMT and IO_MUX were also explicitly mapped as unimplemented devices.
- GP-SPI now reads chip-select and DC signals through the routed-signal layer (`esp32s3_gpio_get_routed_signal_level`) rather than hard-coded GPIO numbers. However, the default signal-to-pin assignments (EPD_CS→GPIO34, EPD_DC→GPIO35, SD_CS→GPIO48, LoRa_CS→GPIO3) are still hardwired in GPIO reset state rather than being derived from firmware-written routing registers.
- SPI2/SPI3 were intentionally a minimum-function stub. Transfers complete instantly, `USR` clears immediately, and `TRANS_DONE` is raised right away instead of following a more realistic controller state progression.
- Clock, reset, and sleep/power behavior are only partially modeled. The `SYSTEM` block handles a small subset of registers, RTC sleep/wake logic is tailored to current timer/GPIO/EXT1 paths, and reset still contains QEMU-specific shims.
- Most previously placeholder peripherals have been tightened to board-path-complete behavior: USB Serial/JTAG now supports RX delivery, FIFO-used status reporting, and RX/TX interrupt state; I2C clears and sets DONE bits per-command with deferred completion IRQ delivery; UART derives baud and pulse timing from the active clock configuration (falling back to 40 MHz only when no clock device is linked); PMS returns RAZ/WI for addresses above 0x100 and a date register at 0xFFC; SHA asserts and clears interrupt state on completion for both DMA and non-DMA paths; RNG is intentionally narrow (only addr==0 && size==4 returns host entropy). Remaining gaps include DMA error paths, uncommon timing modes, unsupported I2C slave/APB-nonfifo modes, and multi-CPU interrupt routing.
- External memory and cache behavior are still functional rather than cycle-accurate. Flash-backed MMU remaps are immediate, but cache sync/preload/autoload requests now complete after a short deferred timer and drive `CACHE_STATE` busy/idle reporting instead of reporting completion only when software reads the control register.
- Some blocks are substituted with generic IP rather than an S3-specific model. Ethernet still uses `open_eth`, but the current tree now makes the QEMU-visible contract explicit: `MIICOMMAND`/MII link polling stays latched correctly, backend link toggles update `MIISTATUS`, and loopback mode can drive descriptor RX from TX for direct regression coverage.
- SPI1 had an outright correctness bug in the flash transfer loop: the byte loop compared the payload value instead of the loop index, making the transfer path data-dependent. (Now fixed; see Stage 2.)
- The generic Xtensa backend still has known accuracy gaps such as remaining reject-surface coverage gaps and unimplemented opcode paths.
- Real `esp32s3` board guest probing is no longer blocked by ROM handoff. The board now loads custom ROM ELFs through `-bios` into each CPU address space correctly, and the tree now has checked-in functional regressions for recoverable cache-alias reject handling on both CPU0 and CPU1. The remaining gap is only the broader reject surface beyond the active board path.

### Task 1 Shortcut Inventory (2026-04-04)

Background commits reviewed for the current board-control path:

- `163978001f` introduced the RTC sleep-state register surface and state fields.
- `0f1ef9a87b` added the timer/GPIO light-sleep state machine.
- `367f037177` wired Core 1 `RUNSTALL` and the RTC CPU-stall magic-value path.
- `3e30797dca` connected GPIO wakeup notifications into the SoC board path.
- `e1da504cc2` added EXT1 light-sleep wakeup handling.
- `a7d95fc530` made RTC clock-source writes update the modeled SoC clock state.
- `767ea45d46` added direct qtests for timer wakeup, reset transitions, and CPU stall.

Remaining shortcuts that still mask real state transitions:

- Reset requests still travel through a QEMU-global shim instead of an explicit ESP32-S3 reset tree. `RTC_CNTL` pulses GPIO lines, the SoC translates them into `qemu_system_reset_request(...)`, and `esp32s3_soc_reset()` reconstructs the intended local effect afterwards.
- Digital-reset fanout is still selective and implicit. `esp32s3_soc_reset()` only cold-resets the interrupt matrix, UARTs, and I2C controllers, while the rest of the digital surface keeps whatever state it had unless some other path resets it.
- Light sleep is still bookkeeping rather than a board-wide power transition. Entering sleep sets `sleeping`, arms the timer, and records wake/reject causes, but it does not gate clocks, pause CPUs, or suspend peripheral activity.
- Clock switching is collapsed to an immediate register rewrite. RTC clock updates directly rewrite `SYSTEM_SYSCLK_CONF` and recompute CPU/APB rates without modeling oscillator enable, PLL lock, divider settling, or source-switch latency.
- Clock-rate fanout is still narrow. The active modeled rate only propagates into CPU clocks and UART timing; most APB-frequency-sensitive peripherals and timers still behave as if their local timing is fixed.
- The RTC-to-clock handoff still exists in two places. The RTC block now updates the clock model directly and also emits the older `clk-update` pulse, so the SoC callback remains as a compatibility shim rather than a single explicit ownership path.
- RTC reset behavior is still shallow. The reset hook re-bases the RTC time counter, but it does not yet rebuild a domain-aware reset/default state for the wider RTC register surface.

### Task 2 Cache/MMU Dependency Inventory (2026-04-04)

Background commits reviewed for the current cache/MMU path:

- `81f8593bc5` introduced the ESP32-S3 cache/MMU model, including MMU entry storage, flash page fill, and IOMMU-backed translation.
- `e0bfd3961f` moved the cache block onto the newer three-stage reset path but did not materially broaden the guest-visible cache/MMU sequence.

Current firmware and library code rely on a narrower subset of cache/MMU behavior than the full `EXTMEM` register surface suggests:

- Boot and OTA code rely on the live MMU table contents being readable at `0x600C5000` with the ESP32-S3 `64 KB` page format. `esp-hal-ota` determines the currently running partition by taking a function pointer, deriving the MMU entry index from the executing virtual address, checking the invalid bit, and reconstructing the backing flash address from the table entry.
- Boot-time PSRAM bring-up relies on DBUS MMU programming rather than only on the static flash mapping. `esp-hal` scans the MMU table for the last mapped flash page, suspends DCache, calls `cache_dbus_mmu_set(...)` to install SPIRAM mappings, clears the DBUS shut bits in `EXTMEM_DCACHE_CTRL1`, and resumes DCache before the allocator starts placing large buffers in PSRAM.
- Runtime flash services rely on the ROM flash operations, not on a higher-level filesystem abstraction alone. `esp-storage` issues `esp_rom_spiflash_read`, `unlock`, `erase_sector`, and `write`, so the emulator needs working flash-backed translation and MMU-backed visibility during both boot and later filesystem or OTA traffic.
- Multi-core flash writes and erases rely on explicit Core 1 parking around the ROM flash calls. The active firmware uses `FlashStorage::multicore_auto_park()` for littlefs and BLE OTA, so the guest-visible contract we care about first is "park the other core, perform the flash op, then unpark" rather than a cycle-accurate cache-disable implementation.
- Panic-path crashdump writes rely on the same raw flash path while assuming the other core is already stalled. That means the important emulation surface is still the flash/MMU path itself, even when the multi-core helper is intentionally bypassed.
- The current reviewed firmware does not appear to rely on the broader EXTMEM management surface such as cache prelock/lock controls, preload/autoload sequencing, PMS reject capture, wraparound control, or cache/MMU fault reporting. Those registers exist in the header today, but they are not part of the confirmed dependency set for the active T-Deck Pro workload.
- There is now direct ESP32-S3 qtest coverage for flash-backed MMU remapping, the `CTRL1` state touched by PSRAM bring-up, and the deferred completion path for sync/preload/autoload operations. Remaining cache/MMU simplifications are now mostly in the "leave cycle-accuracy for later" bucket: coalesced completion timing, simplified freeze semantics, and no attempt to model contention or ROM-internal cache-disable depth.

### Task 3 EMAC Inventory And Current Contract (2026-04-05)

Background commits reviewed for the current Ethernet path:

- `7591824ec4` introduced the ESP32-S3 machine and wired a generic `open_eth` NIC directly into the board at `DR_REG_EMAC_BASE`, its descriptor window at `+0x400`, and `ETS_ETH_MAC_INTR_SOURCE`.
- No later board-specific EMAC commit replaced that wiring. The current tree still instantiates `open_eth` directly from `hw/xtensa/esp32s3.c`.

Current firmware and board-path findings:

- The adjacent T-Deck Pro firmware repo does not contain direct EMAC, Ethernet, RMII, or PHY bring-up code in `src/`. Networking is expressed through `esp_radio::wifi` plus `embassy-net`, not through an Ethernet MAC driver.
- `Cargo.toml` enables `esp-radio` with the `wifi` feature and `embassy-net`, and the runtime path calls `esp_radio::wifi::new(...)` before spawning an `embassy_net::Runner` over `esp_radio::wifi::Interface`.
- There is no app-level evidence that the current firmware reads or writes the ESP32-S3 EMAC register block, configures an external PHY, or expects RMII link state from the board.
- The QEMU board path is correspondingly generic rather than board-specific. It instantiates `open_eth`, maps its two MMIO windows, and routes its interrupt, but it does not model a T-Deck-specific Ethernet PHY or any exercised board wiring around that block.
- The exact EMAC behavior the current firmware touches is therefore effectively none. `open_eth` is a latent fidelity risk for future Ethernet-aware guests, not an actively exercised dependency of the current Wi-Fi-based T-Deck Pro workload.
- Local ESP-IDF context matters here: the bundled `components/esp_eth/src/openeth/esp_eth_mac_openeth.c` driver is already a QEMU-only OpenCores path, so the smallest correct slice is not replacing `open_eth` but making that path explicit and testable.
- Current scope decision: keep `open_eth` for the present board path, but tighten its visible contract instead of leaving it as an untested dormant placeholder.
- Current tree: `open_eth` now latches `MIICOMMAND`, preserves `SCANSTAT`-driven link polling behavior across host reads, updates `MIISTATUS.LINKFAIL` when the backend link changes, and supports descriptor TX-to-RX loopback when `MODER.LOOPBCK` is enabled.
- Current coverage: the ESP32-S3 qtest suite now boots the board with `-nic user,id=emac0,model=open_eth`, uses QMP `set_link` to verify MII-visible link up/down transitions, and proves TX/RX descriptor plus IRQ behavior through loopback.
- Remaining future risk: the board still does not model an ESP32-S3-specific EMAC block or a realistic external PHY beyond the OpenCores/QEMU path. That replacement stays deferred until a guest actually needs more than the current QEMU-targeted contract.

### Task 4 Xtensa Backend Queue (2026-04-04)

The active Xtensa/backend queue now lives in `docs/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md` so it stays separate from the peripheral backlog and tied to current firmware behavior.

Current ranking:

- `P0` Remaining ESP32-S3 reject surface beyond the current qtest and board-regression matrix. The shared illegal-cache path and the first `CORE0/1` reject path now exist, and the tree has checked-in board regressions for both the single-core/core0 path and the SMP/core1 path, but the rest of the reject/write-IC/access-mask matrix is only partially modeled.
- `P1` Remaining missing Xtensa opcodes beyond the current ESP32-S3 core/FPU/TIE coverage. The generic unimplemented-opcode fallback still exists, but the current guest stress instructions (`ee.movi.32.a`, `ee.zero.accx`, `ee.vmulas.s16.accx`) are already translated, so no active firmware blocker is confirmed there yet.

Recently resolved in this track:

- `HELPER(check_atomctl)` now skips `ATOMCTL` cache/PIF gating for accesses that resolve to local DataRAM, matching the `s32c1i` local-memory contract used by ESP32-class Xtensa cores.
- Xtensa softmmu coverage now includes an `esp32s3`-specific `test_s32c1i_atomctl` regression that proves `ATOMCTL=0` still faults on backed sysram (`0x6000_0100`) while succeeding on local DataRAM.
- The `esp32s3` board path now has checked-in functional cache-reject regressions in `tests/functional/test_xtensa_esp32s3_cache_reject.py`, proving that ROM ELFs loaded via `-bios` reach recoverable cache-alias reject handlers on both core0 and core1.
- The Xtensa TCG linker script now places the reset stub at `XCHAL_RESET_VECTOR0_VADDR`, which was required for `esp32`/`esp32s3` softmmu guests to boot on the generic `sim` machine at all.
- The ESP32-S3 cache model now latches `EXTMEM_CACHE_ILG_INT_ST` and `EXTMEM_CACHE_MMU_FAULT_{CONTENT,VADDR}` on invalid MMU accesses, and the board routes the resulting illegal-cache IRQ to `ETS_CACHE_IA_INTR_SOURCE` with direct qtest coverage.
- The ESP32-S3 cache model now also latches the first per-core reject metadata and IRQ path: flash-backed write rejects populate `CORE0/1_ACS_CACHE_INT_ST` plus the matching `CORE0/1_{DBUS,IBUS}_REJECT_{ST,VADDR}` registers, the board routes those lines to `ETS_CACHE_CORE0/1_ACS_INTR_SOURCE`, and the qtest suite covers both core0 DBUS and core0 IBUS reject assert/clear contracts.
- The board path now also tolerates one-core softmmu runs. `esp32s3 -smp 1` no longer crashes in `esp32s3_soc_reset()` by touching unrealized CPU1 state, and the qtest suite has a direct single-CPU boot smoke test to hold that line.

### Current Risk Notes

- The highest remaining accuracy risk is no longer the active display path. The bigger remaining problems are the deferred foundation items: clock/reset/sleep fidelity, cache/MMU sequencing, generic-MMIO dependence, deeper EMAC fidelity beyond the current OpenCores contract, and broader Xtensa accuracy.
- Performance-sensitive guest code is likely to diverge from hardware because cache and MMU operations are modeled as functional state transitions rather than realistic completion and contention behavior.
- Firmware that touches only the currently exercised board path is much more likely to work than firmware that depends on deep power-management, uncommon DMA/peripheral corner cases, EMAC behavior beyond the current OpenCores contract, RMT, or architectural edge conditions.

### Recently Resolved

- **GDMA descriptor-address reconstruction**: The EPD black-screen regression was caused by incorrect DMA RAM buffer-address handling in descriptor reads. Fixed to use ESP32-S3 DMA RAM addressing semantics. A new non-destructive `esp_gdma_read_channel_data` function avoids modifying descriptor owner bits or channel link state during reads.
- **SHA DMA path**: `esp_sha_continue_dma` reads GDMA OUT channel data and runs SHA compress blocks. DMA-backed SHA operations are now functional for the exercised command paths.
- **GP-SPI two-phase transfer model**: Transfers now use a two-phase timer (800 ns execute + 1200 ns completion) with up to 4 retries and a clock-only fallback, replacing the old instant-complete model. A `transfer_prefers_fifo` heuristic avoids re-reading stale DMA descriptors for small FIFO-only transfers.

## Prioritized Backlog

### High Impact

- `P0` [DONE] Fix the SPI1 transfer-loop bug so TX/RX bounds use the loop index instead of the payload byte value.
- `P1` [DONE] Replace GP-SPI hard-coded board pin reads with a routed-signal layer backed by GPIO routing state and a minimal IO_MUX model for the current board path.
- `P1` Replace boot-critical generic-MMIO behavior with explicit narrow models for the firmware-touched offsets currently relied on.
- `P1` [DONE] Make GP-SPI completion semantics more faithful than an unconditional immediate `USR` clear plus `TRANS_DONE`.

### Easy Wins

- `P2` [DONE] Add USB Serial/JTAG RX support, FIFO-ready/backpressure semantics, and interrupt updates.
- `P2` [DONE] Tighten I2C completion semantics, DONE-bit handling, ACK error behavior, and explicit rejection of unsupported modes.
- `P2` [DONE] Make UART pulse timing and baud calculation derive from active clock state instead of fixed 40 MHz assumptions.
- `P2` [DONE] Replace PMS raw echo behavior with reset/default/read-as-zero-write-ignore behavior for the currently touched surface.
- `P2` [DONE] Keep RNG host-backed, but restrict it to a small modeled data path instead of broad undefined behavior.
- `P2` [DONE] Wire SHA completion into realistic interrupt assert/clear behavior for existing command paths.

### Deferred

- `P3` Deep clock/reset/sleep fidelity rework to eliminate QEMU-only identity/workaround behavior.
- `P3` Cache/MMU sequencing and timing realism beyond boot-critical paths.
- `P3` Replace `open_eth` with a more ESP32-S3-specific EMAC model if firmware needs more than the current QEMU OpenCores link / packet contract.
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
- Status:
  - Done: SPI1 TX/RX loop bounds fixed; regression test covers payload values both below and above transfer length.

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
  - Done: boot-critical ANA `PLL_DONE` bit (offset 0x40, bit 24) handled as a special case in the `esp32s3_ana_ops` memory region. The surrounding ANA register space at 0x6000e000 remains an echo store with no further special-case handling.

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

### Stage 6: Deferred Foundation

- Expand routed-pin fidelity beyond the current board path.
- Model GP-SPI transfer timing, busy windows, and DMA completion with more realistic sequencing.
- Replace additional generic-MMIO dependencies as firmware begins to touch them.
- Revisit clock tree, cache/MMU, EMAC, and Xtensa backend fidelity once higher-priority peripheral behavior is stable.

### Stage 6 Status

- Captured in the implemented deferred-foundation plan at `docs/plans/implemented/esp32s3-deferred-foundation/plan.md`.

## Source References

- `hw/xtensa/esp32s3.c`
- `hw/gpio/esp32s3_gpio.c`
- `hw/gpio/esp32s3_iomux.c`
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
