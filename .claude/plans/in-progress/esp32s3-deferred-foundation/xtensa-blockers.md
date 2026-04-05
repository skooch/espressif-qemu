# ESP32-S3 Xtensa Backend Blocker Queue

Updated: 2026-04-05

This queue keeps the ESP32-S3 architectural/backend work separate from the peripheral backlog and ties each item to observed or likely guest behavior.

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
- `tests/functional/test_xtensa_esp32s3_cache_reject.py` now turns part of that manual proof into an in-tree regression: the functional suite embeds a minimal ROM ELF probe and proves the single-core/core0 board path exits cleanly through a recoverable cache-alias reject handler on `esp32s3`.
- The current guest stress path already has translator support for `ee.movi.32.a`, `ee.zero.accx`, and `ee.vmulas.s16.accx`, so those SIMD/TIE instructions are not the first blocker.

## Active ranked queue

### P0: Remaining ESP32-S3 reject surface beyond the current qtest and board-regression matrix

- Code evidence: the first per-core reject contract now exists for flash-backed write rejects on the active cache windows, the board now reliably boots custom ROM ELFs via `-bios`, and the tree now has checked-in board coverage for the single-core/core0 path through `tests/functional/test_xtensa_esp32s3_cache_reject.py`. What is still missing is in-tree board coverage for the CPU1 path plus the still-idle reject/write-IC/access-mask bits advertised by `CORE0/1_ACS_CACHE_INT_*`.
- Guest relevance: this is no longer blocked by board ROM takeover or exception delivery, but it is still the remaining architectural surface before future firmware can rely on more than the shared MMU-entry fault path and the currently covered core0 contracts.
- Why this is next: the model, qtests, manual board proof, and a first functional board regression now agree, so the highest-value next slice is to extend that checked-in board proof to CPU1 and only widen the register surface if guest code starts depending on the remaining bits.
- Suggested first slice: add a permanent board regression for the CPU1 reject path, then decide whether the still-uncovered write-IC/access-mask bits need more model work or only more coverage.

### P1: Remaining missing Xtensa opcodes beyond current ESP32-S3 coverage

- Code evidence: `target/xtensa/translate.c` still contains the generic unimplemented-opcode fallback, and `target/xtensa/translate_tie_esp32s3.c` still logs unknown extension cases for untranslated encodings.
- Guest relevance: no currently exercised firmware instruction has been shown to hit those paths.
- Why this is second: the active firmware stress instructions already translate, so this remains a watch item rather than an immediate blocker once the reject-path repro is captured in-tree.
- Suggested first slice: only promote this item if a concrete guest instruction stream reaches an unimplemented opcode path.

## Current non-blockers

- The current T-Deck Pro stress workload does use ESP32-S3 PIE/SIMD instructions, but the specific opcodes in use are already translated.
- The current firmware path does not appear to read or write the EMAC block, so EMAC fidelity is not part of the immediate dependency chain.
- The board `-bios` loader/handoff issue is no longer blocking guest repros: `esp32s3` now resolves BIOS search paths, loads ROM ELFs into each CPU address space directly, and manual `-smp 1`/`-smp 2` probes now reach recoverable guest reject handlers.

## Next recommended move

- Start the next Task 4 slice by extending the new board regression to the CPU1 reject path, because the tree now preserves the core0 proof and the main remaining gap is multi-core guest-visible coverage.
- After that, come back to the remaining reject surface and only widen the model to the still-idle write-IC and access-mask bits if guest code starts reading them.
