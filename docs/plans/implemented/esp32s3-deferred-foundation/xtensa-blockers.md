# ESP32-S3 Xtensa Backend Blocker Queue

Updated: 2026-04-05

This queue keeps the ESP32-S3 architectural/backend work separate from the peripheral backlog and ties each item to observed or likely guest behavior.

Task 4 is complete for the currently confirmed blockers. This file now serves as a residual watch list and implementation record rather than an active sprint backlog.
Its remaining items are now ranked against the broader hardware-target backlog in `docs/plans/in-progress/esp32s3-fidelity-follow-on/plan.md`.

## Current scope choice

- EMAC replacement is explicitly deferred until a guest actually touches the ESP32-S3 EMAC surface.
- The active firmware path in `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/` is Wi-Fi-only, so the next deferred-foundation work should come from backend blockers rather than dormant Ethernet plumbing.

## Resolved background blockers

- `8939070abe` fixed `CPENABLE` after CPU reset so firmware FPU context-save code no longer trips `CoprocessorDisabled` exceptions.
- `27d3ad1531` added the ESP32-S3 Xtensa core configuration to the backend.
- `76f9e1f154` added the ESP32-S3 TIE translator and helper coverage.
- `target/xtensa/op_helper.c` now treats resolved local DataRAM accesses as outside `ATOMCTL` cache/PIF gating, matching the ESP32-S3 `s32c1i` local-memory contract.
- `tests/tcg/xtensa/test_s32c1i_atomctl.S` now covers the guest-visible contract directly on `esp32s3`: `ATOMCTL=0` still faults on backed sysram (`0x6000_0100`) but succeeds on local DataRAM.
- `tests/tcg/xtensa/linker.ld.S` now places the Xtensa TCG reset stub at `XCHAL_RESET_VECTOR0_VADDR`, which was required for `esp32`/`esp32s3` softmmu guests to boot on the generic `sim` machine instead of trapping at reset.
- `hw/misc/esp32s3_cache.c`, `include/hw/misc/esp32s3_cache.h`, `hw/xtensa/esp32s3.c`, and `tests/qtest/esp32s3-test.c` now land the first ESP32-S3 illegal-cache slice: invalid MMU accesses latch `EXTMEM_CACHE_ILG_INT_ST` plus `EXTMEM_CACHE_MMU_FAULT_{CONTENT,VADDR}`, route the resulting IRQ to `ETS_CACHE_IA_INTR_SOURCE`, and have direct qtest coverage for assert and clear behavior.
- `hw/misc/esp32s3_cache.c`, `include/hw/misc/esp32s3_cache.h`, `hw/xtensa/esp32s3.c`, and `tests/qtest/esp32s3-test.c` now land the first per-core access-reject slice: flash-backed write rejects latch the `CORE0/1_ACS_CACHE_INT_*` state plus the matching `CORE0/1_{DBUS,IBUS}_REJECT_{ST,VADDR}` metadata, route to `ETS_CACHE_CORE0/1_ACS_INTR_SOURCE`, and have direct qtest coverage for both core0 DBUS and core0 IBUS reject assert/clear paths.
- `hw/xtensa/esp32s3.c` and `tests/qtest/esp32s3-test.c` now also cover the one-core board path: `esp32s3 -smp 1` no longer crashes in `esp32s3_soc_reset()`, and the qtest suite includes a direct single-CPU boot smoke test.
- `hw/xtensa/esp32s3.c` now fixes the board ROM handoff itself: `-bios` resolves through the BIOS search path, ROM ELFs load into each CPU address space directly, and raw ROM images still fall back to fixed IROM loading. Temporary `esp32s3` softmmu probes now take control reliably on both `-smp 1` and `-smp 2`, and manual board runs confirm recoverable guest DBUS reject handling for both CPU0 and CPU1.
- `tests/functional/test_xtensa_esp32s3_cache_reject.py` now turns that manual proof into in-tree regressions: the functional suite embeds ROM ELF probes for both the single-core/core0 path and the SMP/core1 path, proving that `esp32s3` reaches recoverable cache-alias reject handlers on both guest-visible cores.
- The current guest stress path already has translator support for `ee.movi.32.a`, `ee.zero.accx`, and `ee.vmulas.s16.accx`, so those SIMD/TIE instructions are not the first blocker.

## Residual watch queue

### P0: Remaining ESP32-S3 reject surface beyond the current qtest and board-regression matrix

- Code evidence: the first per-core reject contract now exists for flash-backed write rejects on the active cache windows, the board reliably boots custom ROM ELFs via `-bios`, and the tree now has checked-in board coverage for both the single-core/core0 path and the SMP/core1 path through `tests/functional/test_xtensa_esp32s3_cache_reject.py`. What remains uncovered is the still-idle reject/write-IC/access-mask surface advertised by `CORE0/1_ACS_CACHE_INT_*`.
- Guest relevance: the board-visible reject path is now covered end to end for both guest-visible cores, so the remaining work is only relevant if future firmware starts reading or relying on the wider reject-status matrix.
- Why this is next: the model, qtests, and board regressions now agree on the active reject path, so any follow-on work should be driven by a concrete guest dependency rather than by missing CPU0/CPU1 proof.
- Suggested first slice: keep the current coverage as the baseline and only widen the register surface if guest code starts depending on the remaining write-IC or access-mask bits.

### P1: Remaining missing Xtensa opcodes beyond current ESP32-S3 coverage

- Code evidence: `target/xtensa/translate.c` still contains the generic unimplemented-opcode fallback, and `target/xtensa/translate_tie_esp32s3.c` still logs unknown extension cases for untranslated encodings.
- Guest relevance: no currently exercised firmware instruction has been shown to hit those paths.
- Why this is second: the active firmware stress instructions already translate, so this remains a watch item rather than an immediate blocker once the reject-path repro is captured in-tree.
- Suggested first slice: only promote this item if a concrete guest instruction stream reaches an unimplemented opcode path.

## Current non-blockers

- The current T-Deck Pro stress workload does use ESP32-S3 PIE/SIMD instructions, but the specific opcodes in use are already translated.
- The current firmware path does not appear to read or write the EMAC block, so EMAC fidelity is not part of the immediate dependency chain.
- The board `-bios` loader/handoff issue is no longer blocking guest repros: `esp32s3` now resolves BIOS search paths, loads ROM ELFs into each CPU address space directly, and manual `-smp 1`/`-smp 2` probes now reach recoverable guest reject handlers.

## Re-entry condition

- Task 4 is closed for the currently confirmed guest blockers: the tree now preserves both the core0 and core1 board proofs.
- Only come back to the remaining reject surface if guest code starts reading the still-idle write-IC or access-mask bits.
