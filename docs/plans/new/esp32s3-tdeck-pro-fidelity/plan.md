# ESP32-S3 T-Deck Pro Fidelity Prioritization Plan

**Goal:** Re-rank the remaining `esp32s3` emulator gaps against the actual T-Deck Pro firmware target so the next work lands where it most improves bring-up, stability, and guest-visible correctness.

**Architecture:** Keep the `qemu-esp32s3` fork board-path first. Use the adjacent `tdeck-pro-rust` firmware, local `esp-hal` / `esp-storage` / `esp-radio` sources, bundled ESP-IDF references, and the active Xtensa toolchain as the evidence base for prioritization instead of generic silicon completeness.

**Tech Stack:** C and QEMU device models, Xtensa backend, qtests, functional board tests, adjacent `esp-rs` firmware, local ESP-IDF references

## Status Snapshot

**Created:** 2026-04-05

**Branch:** `tdeck-peripherals`

**HEAD:** `2c1e678f3d`

This plan supersedes the old split between `ESP32S3_EMULATION_GAPS.md` and the residual `xtensa-blockers.md` watch queue for active prioritization. The deferred-foundation plan is complete; this is the new clean backlog for follow-on work.

## Evidence Summary

- The active hardware target is `../tdeck-pro-rust/`, which is heavily `esp-rs` based rather than an ESP-IDF application. `Cargo.toml` enables `esp-hal` with `esp32s3`, `psram`, and `unstable`, `esp-storage`, `esp-hal-ota`, `esp-radio` with `wifi` and `ble`, `esp-rtos`, and `embassy-net`.
- The active firmware uses sleep paths directly. `src/runtime/event_loop.rs` and `src/runtime/boot.rs` call `rtc.sleep_light(...)` and `rtc.sleep_deep(...)`, while `src/sleep/` manages wake routing, pad hold, and CPU stall behavior.
- The active firmware uses flash and cache-sensitive paths directly. `src/storage/flash.rs`, `src/storage/crashdump.rs`, `src/ble/task.rs`, and `src/runtime/boot.rs` all use `FlashStorage::multicore_auto_park()`.
- The `esp-rs` libraries put real pressure on cache/MMU and sleep behavior. Local `esp-storage` explicitly parks the other core before flash writes, `esp-hal` sleep code configures RTC and EXTMEM state for ESP32-S3 sleep entry, and `esp-hal` PSRAM bring-up uses `cache_dbus_mmu_set(...)`, `Cache_Suspend_DCache`, `Cache_Resume_DCache`, and `EXTMEM_DCACHE_CTRL1` shut bits.
- ESP-IDF changes the Ethernet priority picture. The bundled `components/esp_eth/src/openeth/esp_eth_mac_openeth.c` is explicitly a QEMU-only OpenCores path for running IDF apps in QEMU, so deeper EMAC replacement is not the next target unless a guest actually needs it.
- The current T-Deck Pro firmware appears Wi-Fi only. Local source scans show `esp_radio::wifi` plus `embassy-net`, but no active EMAC / RMII / PHY bring-up and no active RMT use.
- The compiler/backend still matters, but as a triggered queue rather than the top backlog. A disassembly of the current firmware shows heavy real use of `s32c1i` and many `ee.*` TIE/SIMD opcodes, yet no concrete guest failure has been tied to an unimplemented opcode after the current Task 4 work.

## Priority Ladder

### P0: Sleep, Clock, and Reset Reliability

Why this is first:

- The active firmware and `esp-hal` both exercise light sleep, deep sleep, wake-source setup, and CPU stall behavior today.
- The current model still treats light sleep mostly as bookkeeping, still uses a reset shim, and only fans clock-rate changes into a narrow slice of the SoC.
- This is the clearest mismatch between the current emulator and the real T-Deck Pro operating model.

First slices:

- [ ] Replace bookkeeping-only light sleep with an explicit board-visible transition that pauses the right CPUs and suppresses the right peripheral activity for the current firmware path.
- [ ] Replace the QEMU-global reset shim with an explicit ESP32-S3-local reset path and documented reset-domain defaults for the currently exercised surface.
- [ ] Expand clock-rate fanout beyond UART timing to the APB-sensitive peripherals and timer paths the current firmware actually observes during sleep, wake, and boot.
- [ ] Add at least one board-path regression that proves a guest sleep or wake sequence is recoverable for the active workload instead of only checking direct MMIO state.

### P1: Burn Down Generic-MMIO Dependence

Why this is next:

- The board still has a generic catch-all MMIO region for unimplemented peripherals.
- The active firmware uses many raw register paths outside the high-level HAL surface, especially around sleep, boot, panic, and debug flows.
- Catch-all echo behavior can make broken behavior look correct long enough to hide the real blocker.

First slices:

- [ ] Inventory the firmware-touched offsets that still land in the generic MMIO echo region during the active T-Deck Pro path.
- [ ] Replace the first boot-critical and sleep-critical offsets with narrow models or explicit RAZ/WI behavior instead of stored readback.
- [ ] Keep the fallback region only for truly out-of-scope addresses and make the remaining active-path hits visible in tests or logs.
- [ ] Add direct regressions for each register group moved out of the catch-all path.

### P1: Cache, MMU, PSRAM, and Flash Contract Hardening

Why this stays high:

- The active firmware depends on flash writes, OTA, crashdump storage, and PSRAM setup right now.
- Local `esp-storage` expects cross-core flash parking semantics, and local `esp-hal` PSRAM setup expects a meaningful DBUS MMU plus cache suspend/resume contract.
- The current model is good enough for bring-up, but it is still functional rather than faithful across the wider flash/cache lifecycle.

First slices:

- [ ] Re-check the flash-write and erase sequence against `FlashStorage::multicore_auto_park()` so cache, park, and unpark behavior stay guest-visible in the same places the libraries expect.
- [ ] Tighten the PSRAM bring-up contract around DBUS MMU programming, `EXTMEM_DCACHE_CTRL1`, and cache suspend/resume so the active `esp-hal` path is explicit rather than incidental.
- [ ] Keep the already-landed reject and fault paths as the baseline, and only widen the remaining `CORE0/1_ACS_CACHE_INT_*` surface if the guest starts reading those bits.
- [ ] Add the next regression at the same layer as the blocker: qtest for direct MMIO contract, or a board-path guest repro if the library-visible behavior only appears there.

### P2: Xtensa Backend Residual Queue

Why this is not higher yet:

- The active firmware build really does emit a broad set of ESP32-S3 TIE/SIMD instructions and many `s32c1i` operations.
- The currently exercised guest path has not yet produced a concrete missing-opcode blocker after the Task 4 fixes.
- The remaining reject-surface work is only relevant if guest code starts consuming the still-idle access-mask or write-IC bits.

First slices:

- [ ] Keep the remaining reject-surface work gated behind a concrete guest dependency.
- [ ] When a new backend issue appears, capture the exact opcode or reject bit from the failing guest binary before implementing anything broader.
- [ ] Land backend fixes with the narrowest regression that proves the guest-visible contract: Xtensa TCG where possible, board-path ROM probe when necessary.

### P3: Defer Until The Workload Demands It

These items remain valid gaps, but they should not displace the higher-pressure paths above without new guest evidence.

- [ ] Deeper EMAC replacement beyond the current `open_eth` contract
- [ ] Full RMT modeling
- [ ] Broader cycle-accuracy or contention work outside a concrete firmware blocker

## Item-By-Item Assessment

### Remaining Xtensa blocker: `CORE0/1_ACS_CACHE_INT_*` residual surface

- Current state: core0 and core1 reject handling is already proven on the board path for the active alias-write fault flow.
- Pressure from target firmware: low today; no evidence the guest reads the remaining write-IC or access-mask bits.
- Priority call: keep as `P2` watch work, not active implementation.

### Remaining Xtensa blocker: unimplemented opcode paths

- Current state: the generic unimplemented-opcode fallback still exists in the backend, and the active firmware emits many `ee.*` opcodes.
- Pressure from target firmware: moderate latent risk, but no current reproduced blocker.
- Priority call: keep as `P2`, but promote immediately if a guest run trips a missing translation.

### Gap: clock / reset / sleep fidelity

- Current state: partially modeled and still board-shortcut heavy.
- Pressure from target firmware and `esp-hal`: high and current.
- Priority call: `P0`.

### Gap: generic MMIO dependence

- Current state: narrower than before, but still able to mask missing register behavior.
- Pressure from target firmware: high because the current app uses raw registers in sleep, boot, panic, debug, and peripheral bring-up paths.
- Priority call: `P1`.

### Gap: cache / MMU / external memory realism

- Current state: boot-capable and much better than before, but still optimistic about sequencing and state.
- Pressure from target firmware and `esp-rs`: high because flash, OTA, crashdump, and PSRAM are live dependencies.
- Priority call: `P1`.

### Gap: Ethernet fidelity beyond `open_eth`

- Current state: the QEMU-targeted OpenCores contract is explicit and tested.
- Pressure from target firmware and local ESP-IDF evidence: low for this workload.
- Priority call: `P3`.

### Gap: RMT

- Current state: unimplemented.
- Pressure from target firmware: none observed in the current tree.
- Priority call: `P3` until the workload changes.

## Sequencing

1. Start with sleep / clock / reset, because that is the clearest active mismatch with the real device behavior and the most likely source of guest-visible instability.
2. Pull generic-MMIO reduction forward whenever the active firmware proves it is leaning on an echoed register instead of a narrow model.
3. Tackle cache / MMU / flash / PSRAM follow-on work as soon as the next blocker is a library-visible storage or memory-sequencing mismatch.
4. Treat Xtensa backend work as a triggered queue: real failing opcode or real missing reject bit first, no proactive translation spree.
5. Leave EMAC depth, RMT, and broader cycle-accuracy deferred until the firmware path changes.

## Exit Criteria

- The repo has one active follow-on plan for ESP32-S3 fidelity instead of split residual backlogs.
- The deferred-foundation plan remains completed and historical.
- Each remaining gap has a clear priority based on the actual T-Deck Pro firmware target rather than generic completeness.
