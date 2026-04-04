# ESP32-S3 Xtensa Backend Blocker Queue

Updated: 2026-04-04

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
- The current guest stress path already has translator support for `ee.movi.32.a`, `ee.zero.accx`, and `ee.vmulas.s16.accx`, so those SIMD/TIE instructions are not the first blocker.

## Active ranked queue

### P0: Remaining ESP32-S3 cache/local-memory reject plumbing

- Code evidence: the first S3 cache-illegal slice now exists for MMU-entry faults, but `include/hw/misc/esp32s3_cache.h` still exposes `CORE0/1_ACS_CACHE_INT_*` and reject-vaddr registers that `hw/misc/esp32s3_cache.c` does not yet populate.
- Guest relevance: this is still not a confirmed blocker for the current firmware path, but it is now the remaining piece of the local-memory/cache-invalid fault surface once firmware depends on per-core reject metadata rather than only the shared MMU-entry fault interrupt.
- Why this is next: the shared illegal interrupt contract is now modeled and routed, so the remaining risk is the per-core access-reject side rather than the absence of any cache-invalid signal at all.
- Suggested first slice: implement the `CORE0/1` DBUS/IBUS reject status and vaddr latching for the active cache windows, then add regression coverage only if firmware or ROM code starts observing those registers.

### P1: Remaining missing Xtensa opcodes beyond current ESP32-S3 coverage

- Code evidence: `target/xtensa/translate.c` still contains the generic unimplemented-opcode fallback, and `target/xtensa/translate_tie_esp32s3.c` still logs unknown extension cases for untranslated encodings.
- Guest relevance: no currently exercised firmware instruction has been shown to hit those paths.
- Why this is third: the active firmware stress instructions already translate, so this remains a watch item rather than an immediate blocker.
- Suggested first slice: only promote this item if a concrete guest instruction stream reaches an unimplemented opcode path.

## Current non-blockers

- The current T-Deck Pro stress workload does use ESP32-S3 PIE/SIMD instructions, but the specific opcodes in use are already translated.
- The current firmware path does not appear to read or write the EMAC block, so EMAC fidelity is not part of the immediate dependency chain.

## Next recommended move

- Start the next Task 4 slice with the remaining ESP32-S3 cache/local-memory reject path, specifically the `CORE0/1` access-reject registers that still sit idle behind the newly landed shared illegal interrupt contract.
- The Xtensa softmmu TCG harness now both accepts `CORE=...` overrides and boots `esp32`/`esp32s3` guests from `RESET_VECTOR0`, so the next backend slice can land with a real `esp32s3` softmmu regression instead of an ad hoc throwaway binary.
