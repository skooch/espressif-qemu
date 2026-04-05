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
- The current guest stress path already has translator support for `ee.movi.32.a`, `ee.zero.accx`, and `ee.vmulas.s16.accx`, so those SIMD/TIE instructions are not the first blocker.

## Active ranked queue

### P0: Board guest delivery of cache-alias write faults on `esp32s3`

- Code evidence: the first real-board investigation uncovered two separate issues. The `esp32s3 -smp 1` path used for focused repros crashed in `esp32s3_soc_reset()` until the machine stopped touching unrealized CPU1 state, and the temporary custom-ROM probe still needs a stable boot handoff before it can conclusively measure the remaining cache-alias fault delivery path on the real board.
- Guest relevance: this blocks any real guest regression for the per-core reject registers and any future firmware path that expects to trap, inspect `CORE0/1_*` reject state, and resume or fail cleanly.
- Why this is next: the register model and qtest coverage are already in place for the board-visible reject metadata, and the one-core host crash is now out of the way, so the missing piece is a stable real-board repro that can distinguish remaining ROM/handoff issues from any true exception-delivery bug.
- Suggested first slice: finish the permanent minimal `esp32s3` softmmu repro so it reliably takes control on the real board path, then re-measure the cache-alias write fault behavior before deciding whether the next fix belongs in the board path, the ROM handoff, or the generic Xtensa exception delivery.

### P1: Remaining ESP32-S3 reject surface beyond the covered core0 paths

- Code evidence: the first per-core reject contract now exists for flash-backed write rejects on the active cache windows, and both core0 DBUS/IBUS paths are covered directly, but core1-specific behavior still lacks direct regression coverage and `CORE0/1_ACS_CACHE_INT_*` still advertises additional reject and mask/write-IC bits that are not yet modeled.
- Guest relevance: this is still not a confirmed blocker for the current firmware path, but it is the remaining architectural surface once the board can actually deliver those cache-alias faults to guest code and firmware starts depending on reject metadata beyond the shared MMU-entry fault path and the covered core0 reject cases.
- Why this is second now: the board/backend exception path has to be fixed before a real core1-visible guest regression can validate or drive the rest of this matrix.
- Suggested first slice: once the `esp32s3` machine reaches a recoverable exception path for cache-alias write faults, add a real core1-visible regression and then broaden the model to the remaining reject/write-IC/access-mask bits only if guest code starts reading them.

### P2: Remaining missing Xtensa opcodes beyond current ESP32-S3 coverage

- Code evidence: `target/xtensa/translate.c` still contains the generic unimplemented-opcode fallback, and `target/xtensa/translate_tie_esp32s3.c` still logs unknown extension cases for untranslated encodings.
- Guest relevance: no currently exercised firmware instruction has been shown to hit those paths.
- Why this is third: the active firmware stress instructions already translate, so this remains a watch item rather than an immediate blocker.
- Suggested first slice: only promote this item if a concrete guest instruction stream reaches an unimplemented opcode path.

## Current non-blockers

- The current T-Deck Pro stress workload does use ESP32-S3 PIE/SIMD instructions, but the specific opcodes in use are already translated.
- The current firmware path does not appear to read or write the EMAC block, so EMAC fidelity is not part of the immediate dependency chain.

## Next recommended move

- Start the next Task 4 slice with the real-board repro itself: the one-core crash is fixed, so the immediate need is a permanent `esp32s3` softmmu harness that reliably takes control early enough to exercise a cache-alias write fault on the actual board path.
- Once that harness is stable, re-measure the fault path, then come back to the remaining reject surface with a real `esp32s3` guest regression and only widen the model to the still-idle write-IC and access-mask bits if guest code starts reading them.
