# ESP32-S3 Deferred Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Break the deferred ESP32-S3 fidelity backlog into smaller implementation slices that can be executed after stage 1 and the immediate peripheral correctness work are stable.

**Architecture:** Keep the current `qemu-esp32s3` fork as the implementation target and split the deferred items into narrow subsystem tracks. Each track should land with direct qtests or another focused regression before it is considered complete.

**Tech Stack:** C (QEMU device models), QEMU Object Model, Xtensa backend, direct qtests

**QEMU fork:** `/Users/skooch/projects/tdeck-pro-rust/qemu-esp32s3/`

## Status Snapshot

**Last reviewed:** 2026-04-04

**Branch:** `tdeck-peripherals`

**HEAD:** `a9d7966e03`

This plan was updated against the current tree, not the original backlog description. Several parts of the deferred foundation work are already partially implemented and should no longer be treated as untouched.

- Task 1 is **partially complete**. The RTC block now models sleep state, wake causes, CPU reset requests, CPU stall requests, and clock-update signaling, and the board wires those paths through the SoC.
- Task 2 is **partially complete**. The cache/MMU model already implements MMU entry storage, invalidation, flash/PSRAM mapping, and IOMMU-backed translation.
- Task 3 is **still pending**. The board still instantiates `open_eth` directly for the active Ethernet path.
- Task 4 is **still pending as a tracked queue**. Xtensa backend gaps still exist, but they are not yet captured in a repo-visible blocker list tied to firmware behavior.

---

## File Map

### QEMU fork
- **Modify:** `hw/xtensa/esp32s3_clk.c` - finish replacing remaining board-control shortcuts with explicit modeled behavior and consume RTC clock updates meaningfully
- **Modify:** `include/hw/xtensa/esp32s3_clk.h` - add any remaining state needed for the clock/reset/sleep path
- **Modify:** `hw/misc/esp32s3_cache.c` - document and tighten the guest-visible cache/MMU sequencing that is already modeled
- **Modify:** `hw/xtensa/esp32s3.c` - finish wiring clock/reset/sleep interactions together and add any missing board glue
- **Modify:** `hw/gpio/esp32s3_gpio.c` - keep RTC wakeup integration aligned with the RTC sleep path
- **Modify:** `hw/misc/esp32s3_rtc_cntl.c` - finish the remaining reset/sleep/wake fidelity work around the existing state machine
- **Modify:** `include/hw/misc/esp32s3_rtc_cntl.h` - track any additional RTC state needed by the remaining board-control work
- **Modify:** `hw/net/` or replacement device model - replace `open_eth` with a more ESP32-S3-specific EMAC path if firmware requires it
- **Modify:** `target/xtensa/` - track guest-observed Xtensa backend gaps as they become firmware blockers

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
- [ ] Inventory the remaining clock and reset shortcuts that are still masking real state transitions.
  Remaining focus: document what is still implicit between `hw/misc/esp32s3_rtc_cntl.c`, `hw/xtensa/esp32s3_clk.c`, and `hw/xtensa/esp32s3.c`.
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
- [ ] Inventory the cache/MMU operations firmware depends on today.
  Remaining focus: reduce this to the exact operations the current firmware uses rather than the broader modeled surface.
- [ ] Separate immediate state changes from any deferred completion semantics.
  Remaining focus: document which completion bits are still optimistic and which operations need a more explicit sequence.
- [ ] Add regressions for the guest-visible ordering guarantees we care about first.
  Current gap: there are no ESP32-S3 qtests that exercise cache/MMU sequencing directly.
- [ ] Leave broader cycle-accuracy work for a later stage.

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
- [ ] Inventory the exact EMAC behavior the firmware touches.
- [ ] Decide whether the first slice is a replacement model or a board adapter around the existing path.
- [ ] Implement the minimum packet and link bring-up behavior that the firmware needs.
- [ ] Add a regression for link bring-up and packet path behavior.

### Task 3 exit criteria

- Firmware no longer depends on a generic device for the active Ethernet path.
- There is a direct regression for the modeled packet surface.

## Task 4: Xtensa Backend Gaps

**Files:**
- Modify: `target/xtensa/`

The backend still has guest-observed architectural gaps. These should stay tracked, explicit, and grounded in firmware requirements.

- [ ] Record which missing instructions or local-memory behaviors are actually blocking guest code.
  Current gap: the repo still has Xtensa TODOs and unimplemented-opcode paths, but there is no ESP32-S3-specific blocker list in `.claude/`.
- [ ] Rank the blockers so the smallest guest-visible fixes land first.
- [ ] Implement one architectural slice at a time with a focused regression for each.
- [ ] Keep this track separate from peripheral work so the dependency chain stays visible.

### Task 4 exit criteria

- Each backend fix corresponds to a specific guest-observed blocker.
- The blocker list stays visible as a tracked queue, not an implicit assumption.

## Exit Criteria

- Stage 1 and the immediate peripheral work remain green.
- Task 1 and Task 2 have direct regressions for their guest-visible behavior, not just modeled code paths.
- Task 3 has either a direct regression or a clearly defined prerequisite for replacing `open_eth`.
- Task 4 has a repo-visible blocker queue that maps backend work to guest-observed failures.
- The next implementation step can be assigned without re-litigating scope.
