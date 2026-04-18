# ESP32-S3 Fidelity Follow-On Plan

## Goal
Restore a single live ESP32-S3 fidelity backlog, align the repo-root gap document with the archived plan history, and close the next highest-pressure gap for the active T-Deck Pro workload.

## Current Phase
Phase 1 in progress: SYSTIMER light-sleep semantics plus plan/doc state normalization.

## Scope
This plan covers:

- plan and documentation state cleanup so `docs/plans/`, `ESP32S3_EMULATION_GAPS.md`, and the archived implementation history agree
- sleep/clock phase-2 follow-on work that remains high-risk for the active firmware path
- trigger-driven cache/MMU and Xtensa follow-ons only when the guest proves they are needed

This plan does not claim full silicon fidelity. Analog PLL dynamics, full power-domain sequencing, cache microarchitecture, PMS enforcement, physical RNG behavior, and speculative Xtensa configured-core/TIE completeness remain source-gated.

## Evidence Base
- Archived execution history: `docs/plans/implemented/esp32s3-core-soc-fidelity/`, `docs/plans/implemented/esp32s3-tdeck-pro-fidelity/`, `docs/plans/implemented/esp32s3-deferred-foundation/`
- Current gap inventory: `ESP32S3_EMULATION_GAPS.md`
- Adjacent firmware sleep contract:
  - `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/src/runtime/sleep_manager.rs`
  - `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/src/sleep/systimer.rs`
- Local ESP-IDF sleep sources:
  - `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/esp_hw_support/port/esp32s3/rtc_sleep.c`

## File Map
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Modify: `docs/plans/implemented/esp32s3-tdeck-pro-fidelity/plan.md`
- Modify: `docs/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md`
- Create: `docs/plans/in-progress/esp32s3-fidelity-follow-on/plan.md`
- Create: `docs/plans/in-progress/esp32s3-fidelity-follow-on/progress.md`
- Modify: `include/hw/timer/esp_systimer.h`
- Modify: `hw/timer/esp_systimer.c`
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `tests/qtest/esp32s3-test.c`

## Verification Floor
Every implementation slice in this plan must keep the following green unless the slice documents a concrete blocker:

- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_sleep_wake.py`

## Phase 0: Plan And Doc State Normalization
- [x] Create one active `in-progress` ESP32-S3 fidelity plan and progress log.
- [x] Retarget repo-root documentation and residual watch queues to that active plan.
- [x] Mark archived plan files as historical where they still mention execution-era `in-progress` paths.
- [x] Refresh stale gap-document text that still describes already-closed light-sleep/reset shortcuts as open.

## Phase 1: SYSTIMER Light-Sleep Semantics
- [x] Model the ESP32-S3 SYSTIMER as stopped while the SoC is in modeled light sleep.
- [x] Resume SYSTIMER counters and comparator scheduling cleanly on wake without manufacturing slept time inside the hardware model.
- [x] Add direct qtest coverage proving SYSTIMER stays flat during modeled light sleep and resumes counting only after wake.
- [x] Update `ESP32S3_EMULATION_GAPS.md` with the new boundary: SYSTIMER stop/resume is modeled for the active sleep path, but broader oscillator and peripheral-gating realism remains blocked.
- [x] Run the verification floor.

## Phase 2: Clock/Reset/Sleep Follow-On Queue
- [x] Remove duplicated RTC-to-clock ownership so the SoC has one explicit clock-update path.
- [ ] Rebuild RTC reset/default handling around the explicitly modeled register surface instead of only rebasing time.
- [ ] Expand clock/power fanout only where the current guest proves a dependency.

## Triggered Queues
- [ ] Keep residual `CORE0/1_ACS_CACHE_INT_*` widening gated behind concrete guest reads of the still-idle reject/write-IC/access-mask bits.
- [ ] Keep broader Xtensa opcode work gated behind a reproduced failing opcode path or exact configured-core reference.

## Exit Criteria
- The repository has one live ESP32-S3 fidelity plan under `docs/plans/in-progress/`.
- The repo-root gap document no longer points at archived paths or stale “still open” shortcut text.
- The first sleep/clock follow-on slice lands with direct regression coverage and the verification floor recorded in progress.
- Phase 2 keeps a single SoC-owned RTC clock-apply path, with RTC-side bookkeeping only driving the `clk_update` signal.
