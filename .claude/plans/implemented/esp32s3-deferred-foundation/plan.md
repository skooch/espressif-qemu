# ESP32-S3 Deferred Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Break the deferred ESP32-S3 fidelity backlog into smaller implementation slices that can be executed after stage 1 and the immediate peripheral correctness work are stable.

**Architecture:** Keep the current `qemu-esp32s3` fork as the implementation target and split the deferred items into narrow subsystem tracks. Each track should land with direct qtests or another focused regression before it is considered complete.

**Tech Stack:** C (QEMU device models), QEMU Object Model, Xtensa backend, direct qtests

**QEMU fork:** `/Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3/`

## Status Snapshot

**Last reviewed:** 2026-04-05

**Branch:** `tdeck-peripherals`

**HEAD:** `2c1e678f3d`

This plan was updated against the current tree, not the original backlog description. Several parts of the deferred foundation work are already implemented and should no longer be treated as untouched.

This implemented plan is now the source of truth for the completed deferred-foundation track. The temporary repo-root `TODO.md` tracker has been retired.
Follow-on prioritization now lives in `.claude/plans/new/esp32s3-tdeck-pro-fidelity/plan.md`. This file remains the historical record for the completed deferred-foundation stage.

- Task 1 is **complete for this stage**. The RTC block now models sleep state, wake causes, CPU reset requests, CPU stall requests, and clock-update signaling, and the board wires those paths through the SoC with direct qtest coverage for wake, reset, and stall behavior.
- Task 2 is **complete for this stage**. The cache/MMU model now exposes the guest-visible mapping and completion semantics the active workload depends on, with direct qtest coverage for remapping, `CTRL1`, deferred cache-op completion, MMU faults, and the first reject paths.
- Task 3 is **now closed for the current workload**. The board still instantiates `open_eth`, but the minimum guest-visible EMAC contract is now explicit: link-state MII polling latches correctly, backend link toggles propagate into the modeled MAC/PHY surface, and descriptor TX can loop back into RX for direct board-path regression coverage.
- Task 4 is **complete for the currently ranked blocker set**. The backend blockers are captured in `.claude/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md`, and the tree now has direct regressions for the `ATOMCTL` local-memory fix plus board-path cache-reject handling on both core0 and core1.

---

## File Map

### QEMU fork
- **Modify:** `hw/xtensa/esp32s3_clk.c` - finish replacing remaining board-control shortcuts with explicit modeled behavior and consume RTC clock updates meaningfully
- **Modify:** `include/hw/xtensa/esp32s3_clk.h` - add any remaining state needed for the clock/reset/sleep path
- **Modify:** `hw/misc/esp32s3_cache.c` - document and tighten the guest-visible cache/MMU sequencing that is already modeled
- **Modify:** `include/hw/misc/esp32s3_cache.h` - keep the ESP32-S3 cache/MMU register layout aligned with the guest-visible illegal-access surface
- **Modify:** `hw/xtensa/esp32s3.c` - finish wiring clock/reset/sleep interactions together and add any missing board glue
- **Modify:** `hw/gpio/esp32s3_gpio.c` - keep RTC wakeup integration aligned with the RTC sleep path
- **Modify:** `hw/misc/esp32s3_rtc_cntl.c` - finish the remaining reset/sleep/wake fidelity work around the existing state machine
- **Modify:** `include/hw/misc/esp32s3_rtc_cntl.h` - track any additional RTC state needed by the remaining board-control work
- **Modify:** `hw/net/opencores_eth.c` - tighten the generic OpenCores path to the minimum guest-visible EMAC contract used by QEMU-targeted ESP-IDF guests
- **Modify:** `target/xtensa/` - track guest-observed Xtensa backend gaps as they become firmware blockers
- **Modify:** `tests/qtest/esp32s3-test.c` - keep direct ESP32-S3 regression coverage aligned with the guest-visible cache/MMU and board-control contract
- **Add:** `.claude/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md` - repo-visible ranked queue for Xtensa/backend blockers tied to current firmware behavior
- **Add:** `tests/tcg/xtensa/test_s32c1i_atomctl.S` - focused Xtensa softmmu regression for `ATOMCTL` local-memory exclusion on `esp32s3`
- **Modify:** `tests/tcg/xtensa/linker.ld.S` - place the TCG reset stub at `RESET_VECTOR0` so `esp32`/`esp32s3` softmmu guests boot from the actual reset PC

---

## Task 1: Clock, Reset, and Sleep Fidelity

**Files:**
- Modify: `hw/xtensa/esp32s3_clk.c`
- Modify: `include/hw/xtensa/esp32s3_clk.h`
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `hw/misc/esp32s3_rtc_cntl.c`
- Modify: `include/hw/misc/esp32s3_rtc_cntl.h`

This track removes the remaining QEMU-only shortcuts from the board control path and turns the clock/reset/sleep behavior into explicit model state.

- [x] Land the first guest-visible reset/sleep slices in the RTC and board path.
  Current tree: CPU reset requests, CPU stall requests, sleep state, timer wakeup, GPIO wakeup, EXT1 wakeup, and reset-cause/state storage are modeled in the RTC block and wired into the board.
- [x] Keep the first pass board-path-complete rather than trying to cover the full silicon tree.
  Current tree: the path is centered on firmware-visible board control rather than a full silicon reset tree.
- [x] Inventory the remaining clock and reset shortcuts that are still masking real state transitions.
  Current tree: the remaining reset shim, partial digital-reset fanout, bookkeeping-only sleep transitions, collapsed clock switching, narrow clock-rate fanout, duplicate RTC-to-clock update path, and shallow RTC reset behavior are now captured in `ESP32S3_EMULATION_GAPS.md`.
- [x] Finish the clock-update path so RTC clock changes affect the parts of the board model that depend on them.
  Current tree: RTC clock-update pulses now synchronize the SoC clock model so guest-visible consumers like UART timing follow the selected RTC clock source.
- [x] Add direct qtests for the wake, reset, and stall transitions that firmware actually observes.
  Current tree: direct ESP32-S3 qtests now cover timer wakeup, software reset causes with RESET events, and RTC CPU-stall output transitions.

### Task 1 exit criteria

- The remaining board-control path is explicit rather than implicit.
- Reset, wake, and stall transitions are exercised by direct regression coverage.

## Task 2: Cache and MMU Sequencing

**Files:**
- Modify: `hw/misc/esp32s3_cache.c`
- Modify: `target/xtensa/`

Cache and MMU behavior is currently good enough to boot but not yet shaped around realistic sequencing or guest-visible timing.

- [x] Land the basic guest-visible cache/MMU substrate.
  Current tree: MMU entry writes, invalidation, flash page fill, PSRAM selection, cache control bits, and IOMMU translation are present.
- [x] Inventory the cache/MMU operations firmware depends on today.
  Current inventory: the active workload depends on live MMU-table visibility, ROM flash ops, Core 1 parking around flash writes/erases, and the boot-time PSRAM DBUS mapping path much more than on the broader EXTMEM register surface.
- [x] Separate immediate state changes from any deferred completion semantics.
  Current behavior: sync/preload/autoload requests now clear DONE on write, hold busy state in `CACHE_STATE`, and complete on a short virtual timer instead of completing only when software reads the control register.
- [x] Add regressions for the guest-visible ordering guarantees we care about first.
  Current coverage: qtests now exercise flash-backed MMU remapping and the `CTRL1` state used by PSRAM bring-up; the remaining direct gap is completion sequencing for sync/preload/autoload/freeze operations.
- [x] Leave broader cycle-accuracy work for a later stage.
  Deferred scope: independent per-operation timing, contention, ROM-internal cache-disable depth, and more detailed freeze behavior remain intentionally out of scope for this stage.

### Task 2 exit criteria

- Cache/MMU operations are explicit enough that guest-visible ordering is understandable and testable.
- The guest-observed sequence is captured by a regression.

## Task 3: Tighten The `open_eth` Board Path

**Files:**
- Modify: `hw/net/`
- Modify: `hw/xtensa/esp32s3.c`

The current Ethernet story is still anchored by generic IP. For the current workload, this track makes the existing `open_eth` path explicit and testable rather than replacing it outright.

- [x] Confirm whether this track is still needed.
  Current tree: yes. `hw/xtensa/esp32s3.c` still instantiates `open_eth` for the active Ethernet path.
- [x] Inventory the exact EMAC behavior the firmware touches.
  Current inventory: the active T-Deck Pro firmware path is Wi-Fi-only (`esp_radio::wifi` + `embassy-net`) and shows no direct EMAC, RMII, or PHY usage, so `open_eth` is currently a dormant generic stand-in rather than an exercised board dependency.
- [x] Decide whether the first slice is a replacement model or a board adapter around the existing path.
  Current decision: keep the existing `open_eth` device for now and tighten only the guest-visible link / packet surface that ESP-IDF's QEMU path expects.
- [x] Implement the minimum packet and link bring-up behavior that the firmware needs.
  Current tree: `open_eth` now latches `MIICOMMAND`, keeps `SCANSTAT` active across host reads, updates `MIISTATUS.LINKFAIL` when the backend link changes, and loops TX traffic back into RX when `MODER.LOOPBCK` is enabled so descriptor-driven packet handling is testable without a board-specific EMAC replacement.
- [x] Add a regression for link bring-up and packet path behavior.
  Current coverage: the ESP32-S3 qtest suite now boots `-nic user,id=emac0,model=open_eth`, drives QMP `set_link` up/down transitions through the MII surface, and proves descriptor TX-to-RX loopback plus IRQ assertion and clear behavior on the board path.

### Task 3 exit criteria

- The guest-visible EMAC surface used by QEMU-targeted firmware is explicit and regression-tested.
- A future ESP32-S3-specific EMAC replacement remains optional rather than blocking current board fidelity.

## Task 4: Xtensa Backend Gaps

**Files:**
- Modify: `target/xtensa/`

The backend still has guest-observed architectural gaps. These should stay tracked, explicit, and grounded in firmware requirements.

- [x] Record which missing instructions or local-memory behaviors are actually blocking guest code.
  Current queue: `.claude/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md` now captures the resolved blocker history plus the residual ESP32-S3-specific watch items, including the remaining reject-surface and unconfirmed opcode risk.
- [x] Rank the blockers so the smallest guest-visible fixes land first.
  Current ranking: local-memory exclusion / ATOMCTL semantics first, then cache-invalid/local-memory trap plumbing, then any newly confirmed missing opcodes.
- [x] Implement the first architectural slice with a focused regression.
  Current tree: `HELPER(check_atomctl)` now bypasses `ATOMCTL` gating for accesses that resolve to local DataRAM, and Xtensa softmmu coverage now includes an `esp32s3` `s32c1i` regression that distinguishes sysram faults from local-memory success under `ATOMCTL=0`. The Xtensa softmmu linker script also now uses `RESET_VECTOR0`, which was required for `esp32`/`esp32s3` guests to boot from the actual reset PC on the generic `sim` machine.
- [x] Land the first ESP32-S3 cache-invalid/MMU-fault slice with direct board-visible coverage.
  Current tree: the ESP32-S3 cache model now latches `EXTMEM_CACHE_ILG_INT_ST` and `EXTMEM_CACHE_MMU_FAULT_{CONTENT,VADDR}` on invalid MMU accesses, the SoC routes the cache illegal-access IRQ to `ETS_CACHE_IA_INTR_SOURCE`, and the direct ESP32-S3 qtest suite now covers the resulting status, fault metadata, IRQ assertion, and clear semantics.
- [x] Land the first per-core cache access-reject slice with direct board-visible coverage.
  Current tree: flash-backed write rejects now latch `CORE0/1_ACS_CACHE_INT_ST` plus the matching `CORE0/1_{DBUS,IBUS}_REJECT_{ST,VADDR}` registers, the SoC routes those lines to `ETS_CACHE_CORE0/1_ACS_INTR_SOURCE`, and the direct ESP32-S3 qtest suite now covers both core0 DBUS and core0 IBUS reject assert/clear contracts.
- [x] Remove the one-core board reset crash that was blocking `esp32s3 -smp 1` softmmu repros.
  Current tree: `hw/xtensa/esp32s3.c` now respects `machine->smp.cpus` in reset and ROM/clock CPU wiring, and the ESP32-S3 qtest suite includes a direct `-smp 1` boot smoke test.
- [x] Finish a reliable real-board cache-alias fault repro before relying on `esp32s3` softmmu regressions for the per-core reject state.
  Current tree: `hw/xtensa/esp32s3.c` now resolves `-bios` through the BIOS search path, loads ROM ELFs into each CPU address space directly, and falls back to raw ROM images when the file is not ELF. Temporary custom-ROM probes now take control reliably on both `-smp 1` and `-smp 2`, and manual board runs confirm recoverable guest DBUS reject handling for both CPU0 and CPU1 on cache-alias writes.
- [x] Promote the now-working board cache-alias repro into a first in-tree regression.
  Current tree: `tests/functional/test_xtensa_esp32s3_cache_reject.py` now embeds a minimal ROM ELF probe and exercises the real `esp32s3` board path under semihosting, proving that the fixed `-bios` loader reaches a recoverable core0 cache-alias reject handler in-tree.
- [x] Extend the board cache-alias regression to the core1 reject path.
  Current tree: `tests/functional/test_xtensa_esp32s3_cache_reject.py` now also embeds an SMP/core1 ROM probe that reports over UART, boots the real `esp32s3` board under `-smp 2`, and proves CPU1 reaches a recoverable cache-alias reject handler in-tree instead of relying on temporary manual probes.
- [x] Keep this track separate from peripheral work so the dependency chain stays visible.
  Current structure: the blocker queue lives beside this plan rather than being folded into the peripheral backlog.

### Task 4 exit criteria

- Each backend fix corresponds to a specific guest-observed blocker.
- The blocker list stays visible as a tracked queue, not an implicit assumption.

## Exit Criteria

- Stage 1 and the immediate peripheral work remain green.
- Task 1 and Task 2 have direct regressions for their guest-visible behavior, not just modeled code paths.
- Task 3 has a direct regression for the current guest-visible EMAC contract, and any later replacement of `open_eth` is a separate future choice.
- Task 4 has a repo-visible blocker queue that maps backend work to guest-observed failures.
- This plan is complete; any further work should start from a new plan tied to a concrete guest-visible blocker.
