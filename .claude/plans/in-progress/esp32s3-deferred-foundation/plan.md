# ESP32-S3 Deferred Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Break the deferred ESP32-S3 fidelity backlog into smaller implementation slices that can be executed after stage 1 and the immediate peripheral correctness work are stable.

**Architecture:** Keep the current `qemu-esp32s3` fork as the implementation target and split the deferred items into narrow subsystem tracks. Each track should land with direct qtests or another focused regression before it is considered complete.

**Tech Stack:** C (QEMU device models), QEMU Object Model, Xtensa backend, direct qtests

**QEMU fork:** `/Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3/`

## Status Snapshot

**Last reviewed:** 2026-04-05

**Branch:** `tdeck-peripherals`

**HEAD:** `32d0563ca1`

This plan was updated against the current tree, not the original backlog description. Several parts of the deferred foundation work are already partially implemented and should no longer be treated as untouched.

- Task 1 is **partially complete**. The RTC block now models sleep state, wake causes, CPU reset requests, CPU stall requests, and clock-update signaling, and the board wires those paths through the SoC.
- Task 2 is **partially complete**. The cache/MMU model already implements MMU entry storage, invalidation, flash/PSRAM mapping, and IOMMU-backed translation.
- Task 3 is **explicitly deferred**. The board still instantiates `open_eth`, but the current T-Deck Pro firmware path is Wi-Fi-only, so EMAC replacement work is postponed until a guest actually touches that block.
- Task 4 is **now a tracked queue**. The backend blockers are captured in `.claude/plans/in-progress/esp32s3-deferred-foundation/xtensa-blockers.md` and ranked against the current firmware path.
- Task 4 now has a narrower real-board prerequisite for guest-visible reject regressions on the `esp32s3` machine: the first investigation exposed both a one-core board reset crash and a still-unfinished custom repro handoff, so the next slice needs a reliable `esp32s3` softmmu fault harness before we can definitively measure the remaining cache-alias exception-delivery behavior.

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
- **Modify:** `hw/net/` or replacement device model - replace `open_eth` with a more ESP32-S3-specific EMAC path if firmware requires it
- **Modify:** `target/xtensa/` - track guest-observed Xtensa backend gaps as they become firmware blockers
- **Modify:** `tests/qtest/esp32s3-test.c` - keep direct ESP32-S3 regression coverage aligned with the guest-visible cache/MMU and board-control contract
- **Add:** `.claude/plans/in-progress/esp32s3-deferred-foundation/xtensa-blockers.md` - repo-visible ranked queue for Xtensa/backend blockers tied to current firmware behavior
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

## Task 3: Replace `open_eth`

**Files:**
- Modify: `hw/net/`
- Modify: `hw/xtensa/esp32s3.c`

The current Ethernet story is still anchored by generic IP. This track replaces it with either an ESP32-S3-specific model or a narrow dedicated replacement that matches the board surface firmware expects.

- [x] Confirm whether this track is still needed.
  Current tree: yes. `hw/xtensa/esp32s3.c` still instantiates `open_eth` for the active Ethernet path.
- [x] Inventory the exact EMAC behavior the firmware touches.
  Current inventory: the active T-Deck Pro firmware path is Wi-Fi-only (`esp_radio::wifi` + `embassy-net`) and shows no direct EMAC, RMII, or PHY usage, so `open_eth` is currently a dormant generic stand-in rather than an exercised board dependency.
- [x] Decide whether the first slice is a replacement model or a board adapter around the existing path.
  Current decision: neither for now. By explicit scope choice, this track is deferred until guest firmware actually touches the EMAC surface.
- [ ] Implement the minimum packet and link bring-up behavior that the firmware needs.
  Deferred trigger: start this only when a guest begins reading or writing the EMAC surface or requires link behavior beyond the current dormant `open_eth` stand-in.
- [ ] Add a regression for link bring-up and packet path behavior.
  Deferred with the implementation work above.

### Task 3 exit criteria

- Firmware no longer depends on a generic device for the active Ethernet path.
- There is a direct regression for the modeled packet surface.

## Task 4: Xtensa Backend Gaps

**Files:**
- Modify: `target/xtensa/`

The backend still has guest-observed architectural gaps. These should stay tracked, explicit, and grounded in firmware requirements.

- [x] Record which missing instructions or local-memory behaviors are actually blocking guest code.
  Current queue: `.claude/plans/in-progress/esp32s3-deferred-foundation/xtensa-blockers.md` now captures the active ESP32-S3-specific blocker list, including the ATOMCTL/local-memory exclusion gap, missing cache-invalid trap plumbing, and the remaining unconfirmed opcode risk.
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
- [ ] Finish a reliable real-board cache-alias fault repro before relying on `esp32s3` softmmu regressions for the per-core reject state.
  Current blocker: the one-core board path no longer crashes, but the temporary custom-ROM probe still needs a stable boot/repro handoff before it can prove whether the remaining real-board problem is exception delivery, ROM takeover, or both.
  Current blocker: temporary ROM probes that set MMU entry 0 and then store to `0x3c000000` currently land in the double-exception path with `EXCCAUSE=15` on both CPU0 and CPU1, so a guest kernel handler never gets a clean chance to observe the already-modeled `CORE0/1_{DBUS,IBUS}_REJECT_*` registers.
- [x] Keep this track separate from peripheral work so the dependency chain stays visible.
  Current structure: the blocker queue lives beside this plan rather than being folded into the peripheral backlog.

### Task 4 exit criteria

- Each backend fix corresponds to a specific guest-observed blocker.
- The blocker list stays visible as a tracked queue, not an implicit assumption.

## Exit Criteria

- Stage 1 and the immediate peripheral work remain green.
- Task 1 and Task 2 have direct regressions for their guest-visible behavior, not just modeled code paths.
- Task 3 has either a direct regression or a clearly defined prerequisite for replacing `open_eth`.
- Task 4 has a repo-visible blocker queue that maps backend work to guest-observed failures.
- The next implementation step can be assigned without re-litigating scope.
