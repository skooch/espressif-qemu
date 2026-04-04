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
- The current guest stress path already has translator support for `ee.movi.32.a`, `ee.zero.accx`, and `ee.vmulas.s16.accx`, so those SIMD/TIE instructions are not the first blocker.

## Active ranked queue

### P0: Local-memory exclusion / ATOMCTL semantics for atomic operations

- Code evidence: `target/xtensa/op_helper.c` still documents `HELPER(check_atomctl)` with `Note: local memory exclusion is not implemented`.
- Guest relevance: the adjacent firmware uses atomics and synchronization primitives heavily across storage, clock control, Wi-Fi orchestration, task monitoring, remote debug, display state, and UI automation.
- Why this is first: it is the clearest backend TODO already called out in code, it is plausibly reachable through current guest synchronization paths, and it is smaller than broader opcode or board-plumbing work.
- Suggested first slice: confirm the exact `s32c1i` or related conditional-store contract the current guest depends on, then land the smallest fix with a focused backend regression.

### P1: ESP32-S3 cache-invalid/local-memory trap plumbing

- Code evidence: `hw/xtensa/esp32.c` wires illegal-access trap memory regions and routes `ETS_CACHE_IA_INTR_SOURCE`, while `hw/xtensa/esp32s3.c` does not currently mirror that path.
- Guest relevance: this is not a confirmed blocker for the current firmware path, but it is the most obvious missing local-memory fault surface near the backend gaps.
- Why this is second: it likely matters once firmware starts relying on cache-invalid/local-memory traps, but it is broader than the `ATOMCTL` helper gap because it crosses machine wiring and interrupt routing.
- Suggested first slice: inventory the exact ESP32-S3 register and interrupt behavior needed for cache-invalid/local-memory faults before lifting the ESP32 pattern over directly.

### P2: Remaining missing Xtensa opcodes beyond current ESP32-S3 coverage

- Code evidence: `target/xtensa/translate.c` still contains the generic unimplemented-opcode fallback, and `target/xtensa/translate_tie_esp32s3.c` still logs unknown extension cases for untranslated encodings.
- Guest relevance: no currently exercised firmware instruction has been shown to hit those paths.
- Why this is third: the active firmware stress instructions already translate, so this remains a watch item rather than an immediate blocker.
- Suggested first slice: only promote this item if a concrete guest instruction stream reaches an unimplemented opcode path.

## Current non-blockers

- The current T-Deck Pro stress workload does use ESP32-S3 PIE/SIMD instructions, but the specific opcodes in use are already translated.
- The current firmware path does not appear to read or write the EMAC block, so EMAC fidelity is not part of the immediate dependency chain.

## Next recommended move

- Start Task 4 implementation with the `ATOMCTL` / local-memory exclusion path, but only after capturing the exact guest-visible atomic contract that is missing so the first fix lands with a narrow regression instead of a speculative backend change.
- The Xtensa softmmu TCG harness now accepts `CORE=...` overrides via `tests/tcg/xtensa/Makefile.softmmu-target`, so the next regression step is to bring in an Xtensa guest cross toolchain and run that harness with `CORE=esp32s3`.
