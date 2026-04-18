# ESP32-S3 T-Deck Pro Fidelity Prioritization Plan

**Goal:** Re-rank the remaining `esp32s3` emulator gaps against the actual T-Deck Pro firmware target so the next work lands where it most improves bring-up, stability, and guest-visible correctness.

**Architecture:** Keep the `qemu-esp32s3` fork board-path first. Use the adjacent `tdeck-pro-rust` firmware, local `esp-hal` / `esp-storage` / `esp-radio` sources, bundled ESP-IDF references, and the active Xtensa toolchain as the evidence base for prioritization instead of generic silicon completeness.

**Tech Stack:** C and QEMU device models, Xtensa backend, qtests, functional board tests, adjacent `esp-rs` firmware, local ESP-IDF references

## Status Snapshot

**Created:** 2026-04-05

**Branch:** `codex/esp32s3-p1-generic-mmio`

**HEAD:** `c45da302ce`

This plan supersedes the old split between `ESP32S3_EMULATION_GAPS.md` and the residual `xtensa-blockers.md` watch queue for active prioritization. The deferred-foundation plan is complete; this is the new clean backlog for follow-on work. The P0 branch has been merged to `tdeck-peripherals`; active implementation has moved to the P1 generic-MMIO burn-down branch.

This plan is archived. References below to `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/*` are retained as execution-history paths from the time the work ran. Its follow-on cleanup plan is now archived at `docs/plans/implemented/esp32s3-fidelity-follow-on/plan.md`.

## Active Implementation Slice: P0 Light-Sleep Board Transition

This worktree starts the P0 sleep/clock/reset track with a focused light-sleep transition slice. The first deliverable is not full ESP32-S3 power-domain fidelity; it is a testable board-visible contract that replaces internal-only RTC sleep bookkeeping with an explicit SoC transition.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track P0 slice status and errors)
- Create: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document the new light-sleep transition boundary and remaining blockers)
- Modify: `include/hw/misc/esp32s3_rtc_cntl.h` (add RTC light-sleep output GPIO declaration)
- Modify: `hw/misc/esp32s3_rtc_cntl.c` (emit light-sleep output on sleep enter, wake, reject, and reset)
- Modify: `include/hw/xtensa/esp32s3_clk.h` (expose SYSTEM core1 RUNSTALL as a named output GPIO)
- Modify: `hw/xtensa/esp32s3_clk.c` (emit RUNSTALL output instead of pausing/resuming core1 directly)
- Modify: `hw/xtensa/esp32s3.c` (wire RTC light-sleep output and SYSTEM RUNSTALL into centralized SoC CPU pause/resume state)
- Modify: `tests/qtest/esp32s3-test.c` (add direct regression coverage for light-sleep output, CPU-stall, and RUNSTALL interaction boundaries)

### P0 Slice Tasks

- [x] Move this plan from `docs/plans/new/esp32s3-tdeck-pro-fidelity/` to `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/` before implementation.
- [x] Add a named RTC light-sleep output GPIO that is high only while the RTC state machine is in `ESP32S3_RTC_SLEEP_SLEEPING`.
- [x] Wire the RTC light-sleep output into `hw/xtensa/esp32s3.c` and centralize CPU pause/resume decisions so light sleep, RTC CPU-stall, and SYSTEM core1 RUNSTALL requests do not incorrectly resume each other.
- [x] Preserve timer, GPIO, EXT1 immediate-wake, and reject semantics while emitting the new board-visible sleep transition.
- [x] Add qtests proving timer sleep asserts the light-sleep output until wake, reject never asserts the output, RTC CPU-stall still emits independent per-core hold lines, and SYSTEM core1 RUNSTALL now emits its own SoC-owned hold line.
- [x] Update `ESP32S3_EMULATION_GAPS.md` to describe this as a board-visible light-sleep transition while keeping full power-domain sequencing, peripheral gating, and oscillator timing blocked.
- [x] Run the ESP32-S3 qtest suite and cache-reject functional test before committing.

**Status:** complete; P0 light-sleep transition slice committed.

## Active Implementation Slice: P0 Local Reset Dispatch

The second P0 slice removes the guest software-reset dependency on QEMU process-level reset events. RTC_CNTL software reset bits now dispatch explicit ESP32-S3 reset domains locally through the SoC, while host/QMP `system_reset` remains the full-chip reset entrypoint.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track P0 reset slice status and errors)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document guest-local reset dispatch and remaining reset fidelity limits)
- Modify: `hw/xtensa/esp32s3.c` (replace guest reset QEMU-global reset requests with local reset-domain dispatch)
- Modify: `tests/qtest/esp32s3-test.c` (prove PROCPU, APPCPU, and digital reset effects without synthetic QMP RESET events)

### P0 Reset Slice Tasks

- [x] Replace RTC guest software reset callbacks that call `qemu_system_reset_request()` with an explicit local SoC reset-domain dispatcher.
- [x] Preserve host/QMP full-chip reset through QEMU reset registration, but register it with a typed wrapper instead of a function-pointer cast.
- [x] Preserve PROCPU/APPCPU reset isolation, digital reset peripheral clearing, RTC scratch retention, and guest-visible reset causes.
- [x] Extend qtest reset coverage to include APPCPU software reset and to stop relying on QMP `RESET` events for guest software reset bits.
- [x] Run the full ESP32-S3 qtest suite and cache-reject functional test before committing.

**Status:** complete; reset slice ready to commit.

## Active Implementation Slice: P0 TIMG Clock Fanout

The third P0 slice expands clock-rate fanout from CPU/UART-only behavior into timer-group consumers. TIMG APB-sourced counters and watchdogs now receive the SoC-derived APB frequency, and XTAL-sourced timer paths receive the modeled XTAL frequency.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track P0 clock fanout slice status)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document TIMG clock fanout and remaining clock limits)
- Modify: `include/hw/timer/esp_timg.h` (add dynamic clock update API and state)
- Modify: `hw/timer/esp_timg.c` (use dynamic APB/XTAL frequencies for TIMG counters and watchdogs)
- Modify: `include/hw/xtensa/esp32s3_clk.h` (add SYSTEM clock-update output)
- Modify: `hw/xtensa/esp32s3_clk.c` (emit clock-update when SYSTEM/RTC clock configuration changes)
- Modify: `hw/xtensa/esp32s3.c` (fan out clock updates into both timer groups)
- Modify: `tests/qtest/esp32s3-test.c` (prove TIMG APB counter rate follows SYSTEM clock selection)

### P0 Clock Fanout Tasks

- [x] Add a clock-update output from the SYSTEM clock block for direct SYSTEM writes and RTC-driven clock changes.
- [x] Fan out derived APB and XTAL rates from the SoC clock model into both ESP32-S3 timer groups.
- [x] Preserve enabled TIMG counter/watchdog elapsed ticks across a clock-rate change before applying the new frequency.
- [x] Add a qtest proving a TIMG APB counter advances at 80 ticks/us under PLL/APB=80 MHz and 40 ticks/us after switching SOC clock to XTAL.
- [x] Run the full ESP32-S3 qtest suite and cache-reject functional test before committing.

**Status:** complete; TIMG clock fanout slice ready to commit.

## Active Implementation Slice: P0 Sleep/Wake Board Regression

The fourth P0 slice adds a guest-level regression for the modeled light-sleep path. A minimal ESP32-S3 ROM ELF is loaded through the board `-bios` path, programs RTC timer wake, enters light sleep, resumes after wake, validates the guest-visible wake cause/state, and exits through semihosting.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track P0 board regression status)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document sleep/wake board regression coverage and limits)
- Modify: `tests/functional/meson.build` (register the new Xtensa functional test)
- Add: `tests/functional/test_xtensa_esp32s3_sleep_wake.py` (embedded ROM ELF board-path sleep/wake probe)

### P0 Board Regression Tasks

- [x] Add an ESP32-S3 ROM functional probe that enters RTC timer light sleep through guest MMIO instead of qtest host MMIO.
- [x] Validate after guest resume that `RTC_CNTL_INT_RAW.SLP_WAKEUP`, `RTC_CNTL_SLP_WAKEUP_CAUSE.TIMER`, and `RTC_CNTL_STATE0.SLP_WAKEUP` are guest-visible.
- [x] Register the regression in the Xtensa functional-test list.
- [x] Run the new sleep/wake functional test, full ESP32-S3 qtest suite, and cache-reject functional test before committing.

**Status:** complete; board-path sleep/wake regression ready to commit.

## Active Implementation Slice: P1 APB_SARADC/SENS Generic-MMIO Burn-Down

The first P1 slice removes the remaining source-backed analog-control windows from the generic MMIO echo path. This is deliberately a narrow register-surface shim for firmware-touched APB_SARADC and SENS offsets, not an ADC, calibration, or physical analog model.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track P1 generic-MMIO slice status)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document APB_SARADC/SENS removal from the catch-all path and remaining limits)
- Modify: `hw/xtensa/esp32s3.c` (add explicit APB_SARADC and SENS MMIO regions with source-backed defaults and masks)
- Modify: `tests/qtest/esp32s3-test.c` (pin reset defaults, write masks, unsupported-offset RAZ/WI behavior, and reset restoration)

### P1 APB_SARADC/SENS Tasks

- [x] Replace active `DR_REG_APB_SARADC_BASE` catch-all offsets `0x00`, `0x04`, `0x18`, `0x28`, `0x38`, `0x3c`, and `0x70` with an explicit narrow register model backed by ESP-IDF `apb_saradc_reg.h` reset defaults and write masks.
- [x] Replace active `DR_REG_SENS_BASE` catch-all offsets `0x10`, `0x34`, and `0x3c` with an explicit narrow register model backed by ESP-IDF `sens_reg.h` masks.
- [x] Make unsupported APB_SARADC/SENS offsets deterministic RAZ/WI instead of stored readback.
- [x] Reset the APB_SARADC/SENS shim state through the modeled digital-peripheral reset path.
- [x] Add qtest coverage for documented reset defaults, write-mask behavior, unsupported-offset RAZ/WI behavior, and reset restoration.
- [x] Run the ESP32-S3 qtest suite plus cache-reject and sleep-wake functional tests before committing.

**Status:** complete; APB_SARADC/SENS generic-MMIO burn-down slice ready to commit.

## Active Implementation Slice: P1 Generic-MMIO Fallback Allowlist

The second P1 slice narrows the broad fallback region itself. Instead of storing and echoing every unmapped word in the ESP32-S3 MMIO window, the fallback now preserves compatibility storage only for ranges already classified as out-of-scope for this core-SoC pass.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track second P1 generic-MMIO slice status)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document fallback storage narrowing and remaining limits)
- Modify: `hw/xtensa/esp32s3.c` (restrict generic-MMIO stored readback to LEDC plus FE/FE2/NRX/BB compatibility ranges)
- Modify: `tests/qtest/esp32s3-test.c` (prove compatibility storage remains only for out-of-scope ranges while other core fallthroughs are RAZ/WI)

### P1 Fallback Allowlist Tasks

- [x] Add an explicit compatibility allowlist for `DR_REG_LEDC_BASE`, `DR_REG_FE2_BASE`, `DR_REG_FE_BASE`, `DR_REG_NRX_BASE`, and `DR_REG_BB_BASE` fallback storage.
- [x] Make any other unmapped core-SoC fallthrough deterministic RAZ/WI while preserving existing trace events.
- [x] Update the generic-MMIO qtest so it checks both allowed out-of-scope storage and disallowed WCL fallthrough RAZ/WI behavior.
- [x] Run the ESP32-S3 qtest suite plus cache-reject and sleep-wake functional tests before committing.

**Status:** complete; generic-MMIO fallback allowlist slice ready to commit.

## Active Implementation Slice: P1 EXTMEM Cache Bus Gating

The first P1 cache/MMU hardening slice makes the `esp-hal` PSRAM and flash bring-up contract explicit by honoring per-core bus-shut bits in the EXTMEM cache control registers. MMU alias translation now refuses core0 accesses until the relevant `EXTMEM_DCACHE_CTRL1`/`EXTMEM_ICACHE_CTRL1` bus bit is cleared, matching the documented esp-hal sequence rather than treating the alias as immediately usable after MMU mapping.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track P1 cache/MMU slice status)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document the explicit cache bus gate and the now-active esp-hal contract)
- Modify: `hw/misc/esp32s3_cache.c` (use ESP-IDF-backed CTRL1 field names, reset defaults, MMU alias gating)
- Modify: `include/hw/misc/esp32s3_cache.h` (CTRL1 field declarations and reset defaults)
- Modify: `tests/qtest/esp32s3-test.c` (direct qtests for flash/PSRAM aliases blocked until CTRL1 bus bit is cleared)

### P1 EXTMEM Cache Bus Gating Tasks

- [x] Use ESP-IDF-backed per-core `CTRL1` field names and reset defaults in the cache model.
- [x] Refuse core0 alias translation until the relevant bus bit is cleared.
- [x] Cover flash-IBUS and PSRAM-DBUS gating with direct qtests (`/xtensa/esp32s3/cache/ctrl1-flash-ibus-gate`, `/xtensa/esp32s3/cache/ctrl1-psram-dbus-gate`).
- [x] Run the ESP32-S3 qtest suite plus cache-reject and sleep-wake functional tests before committing.

**Status:** complete; EXTMEM cache bus gating slice committed as `121ab7e03f`.

## Active Implementation Slice: P1 SPI1 Mapped Flash Refresh

The second P1 cache/MMU hardening slice closes a coherency gap exposed by a controlled qtest repro: SPI1 page program updated the flash backend, but the mapped EXTMEM alias stayed stale until remap. SPI1 flash-mutating commands now refresh affected mapped flash pages so guests that park the other core and mutate flash through SPI1 see the updated contents through the cache alias without a forced MMU remap. This is the direct-MMIO half of the `FlashStorage::multicore_auto_park()` contract; board-path evidence for the multicore-park sequence itself is still pending.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track second P1 cache/MMU slice status)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document SPI1 mapped flash refresh and remaining limits)
- Modify: `hw/ssi/esp32s3_spi.c` and `include/hw/ssi/esp32s3_spi.h` (notify EXTMEM cache on flash-mutating SPI1 commands)
- Modify: `hw/misc/esp32s3_cache.c` and `include/hw/misc/esp32s3_cache.h` (refresh affected mapped flash pages on program/erase notifications)
- Modify: `hw/xtensa/esp32s3.c` (wire SPI1 cache notifications into the SoC cache model)
- Modify: `tests/qtest/esp32s3-test.c` (direct qtest proving SPI1 reads and mapped cache alias stay coherent across program and sector erase)

### P1 SPI1 Mapped Flash Refresh Tasks

- [x] Wire SPI1 flash-mutating commands into the EXTMEM cache model so page-program and erase refresh affected mapped flash pages.
- [x] Add a direct qtest (`/xtensa/esp32s3/cache/flash-spi-updates-mapped-alias`) that proves SPI1 reads and the mapped cache alias stay coherent across program and sector erase.
- [x] Run the ESP32-S3 qtest suite plus cache-reject and sleep-wake functional tests before committing.

**Status:** complete; SPI1 mapped flash refresh slice committed as `ae274a4452`.

## Active Implementation Slice: P1 Multicore-Park Flash Coherency Board Regression

The third P1 cache/MMU hardening slice promotes the direct-MMIO SPI1 mapped-flash coherency qtest (`ae274a4452`) into a guest-driven board regression that exercises the full `FlashStorage::multicore_auto_park()` contract local firmware uses. The test boots an ESP32-S3 ROM ELF via `-bios` with `-smp 2`, brings up the APP core with a shared-DRAM counter, parks the APP core through the exact `esp-hal` primitives (`SYSTEM.CORE_1_CONTROL_0` clkgate/runstall and RTC_CNTL `OPTIONS0`+`SW_CPU_STALL` magic stall), confirms the APP core is paused via the counter, mutates flash through SPI1 page program and sector erase from the still-running PRO core, validates the mapped EXTMEM alias reflects the new flash contents at each phase, unparks the APP core, and confirms it resumes before exiting through semihosting.

### File Map

- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/plan.md` (track third P1 cache/MMU slice status)
- Modify: `docs/plans/in-progress/esp32s3-tdeck-pro-fidelity/progress.md` (session log)
- Modify: `ESP32S3_EMULATION_GAPS.md` (document multicore-park flash coherency board-path coverage)
- Modify: `tests/functional/meson.build` (register new functional test)
- Add: `tests/functional/test_xtensa_esp32s3_flash_multicore_park.py` (board-path regression for `FlashStorage::multicore_auto_park()` flash coherency contract)

### P1 Multicore-Park Flash Coherency Board Regression Tasks

- [x] Build a two-core ROM ELF that clears EXTMEM ICACHE core0-bus-shut, installs an MMU entry mapping flash page 0 into the ICACHE alias, brings up the APP core on a shared-DRAM counter loop, and parks it through RTC_CNTL `OPTIONS0`+`SW_CPU_STALL` stall-magic (the same primitive `esp-hal` `park_other_core()` uses; the `SYSTEM.CORE_1_CONTROL_0` clkgate/runstall path is redundant here and omitted to keep the ROM minimal).
- [x] From the still-running PRO core, mutate the mapped flash page via SPI1 WREN + page program and validate the ICACHE alias reflects the new payload; then erase the sector via SPI1 WREN + SE and validate the alias reflects `0xffffffff`.
- [x] Confirm the APP-core counter is frozen across both mutations and resumes advancing after unparking; exit through semihosting with a deterministic pass/fail code.
- [x] Embed the ROM as zlib+base64 in a new functional test modelled on `test_xtensa_esp32s3_sleep_wake.py` and register it in `tests/functional/meson.build`. The test also creates a pre-erased 2 MiB `-drive if=mtd` backing image so the guest's SPI1 page-program sees NOR-erased cells, matching the `create_erased_flash_image()` pattern in `tests/qtest/esp32s3-test.c`.
- [x] Run the verification gate before committing: `git diff --check`, incremental `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, the new functional test, the `flash-spi-updates-mapped-alias` / `ctrl1-flash-ibus-gate` / `ctrl1-psram-dbus-gate` qtests, the full `esp32s3-test` qtest suite, and `test_xtensa_esp32s3_cache_reject.py` and `test_xtensa_esp32s3_sleep_wake.py`.

**Status:** complete; the multicore-park flash coherency board regression lands as `tests/functional/test_xtensa_esp32s3_multicore_park_flash.py`. The embedded ROM parks the APP core through the RTC_CNTL stall-magic, mutates flash via SPI1 WREN+PP and WREN+SE with the APP-core counter frozen, verifies the EXTMEM ICACHE alias reflects each mutation, unparks the APP core, and confirms its counter resumes before a semihosted simcall exits with code 0. One open item worth noting: the reset-vector boot-stub installer in `esp32s3_load_bios()` fans out to all CPUs, but the `-kernel` path in `esp32s3_machine_init` only installs the stub on cpu[0]; the new board regression therefore uses `-bios` rather than `-kernel`, matching the existing cache-reject/sleep-wake tests.

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

- [x] Replace bookkeeping-only light sleep with an explicit board-visible transition that pauses the right CPUs and suppresses the right peripheral activity for the current firmware path.
- [x] Replace the QEMU-global reset shim with an explicit ESP32-S3-local reset path and documented reset-domain defaults for the currently exercised surface.
- [x] Expand clock-rate fanout beyond UART timing to the APB-sensitive peripherals and timer paths the current firmware actually observes during sleep, wake, and boot.
- [x] Add at least one board-path regression that proves a guest sleep or wake sequence is recoverable for the active workload instead of only checking direct MMIO state.

### P1: Burn Down Generic-MMIO Dependence

Why this is next:

- The board still has a generic catch-all MMIO region for unimplemented peripherals.
- The active firmware uses many raw register paths outside the high-level HAL surface, especially around sleep, boot, panic, and debug flows.
- Catch-all echo behavior can make broken behavior look correct long enough to hide the real blocker.

First slices:

- [x] Inventory the firmware-touched offsets that still land in the generic MMIO echo region during the active T-Deck Pro path.
- [x] Replace the first boot-critical and sleep-critical offsets with narrow models or explicit RAZ/WI behavior instead of stored readback.
- [x] Keep the fallback region only for truly out-of-scope addresses and make the remaining active-path hits visible in tests or logs.
- [x] Add direct regressions for each register group moved out of the catch-all path.

### P1: Cache, MMU, PSRAM, and Flash Contract Hardening

Why this stays high:

- The active firmware depends on flash writes, OTA, crashdump storage, and PSRAM setup right now.
- Local `esp-storage` expects cross-core flash parking semantics, and local `esp-hal` PSRAM setup expects a meaningful DBUS MMU plus cache suspend/resume contract.
- The current model is good enough for bring-up, but it is still functional rather than faithful across the wider flash/cache lifecycle.

First slices:

- [x] Re-check the flash-write and erase sequence against `FlashStorage::multicore_auto_park()` so cache, park, and unpark behavior stay guest-visible in the same places the libraries expect. (Direct qtest for mapped-alias coherency landed as `flash-spi-updates-mapped-alias`; board-path multicore-park regression landed as `test_xtensa_esp32s3_multicore_park_flash.py`.)
- [x] Tighten the PSRAM bring-up contract around DBUS MMU programming, `EXTMEM_DCACHE_CTRL1`, and cache suspend/resume so the active `esp-hal` path is explicit rather than incidental. (EXTMEM cache bus gating slice landed earlier; the suspend/resume depth slice now routes `EXTMEM_DCACHE_FREEZE`/`EXTMEM_ICACHE_FREEZE` through the deferred-completion infrastructure so `FREEZE_DONE` defers until the domain's non-freeze ops drain, matching the ROM `Cache_Suspend_DCache()` / `Cache_Wait_Idle(0)` polling contract, and logs DBUS MMU rewrites for PSRAM entries when DCACHE is not suspended so the `esp-hal` contract violation is at least observable without inventing a divergent hardware signal.)
- [ ] Keep the already-landed reject and fault paths as the baseline, and only widen the remaining `CORE0/1_ACS_CACHE_INT_*` surface if the guest starts reading those bits.
- [ ] Add the next regression at the same layer as the blocker: qtest for direct MMIO contract, or a board-path guest repro if the library-visible behavior only appears there.

Current P1 cache/MMU progress:

- 2026-04-16: The EXTMEM cache model now resets `DCACHE_CTRL1`/`ICACHE_CTRL1` to the ESP-IDF documented per-core bus-shut defaults and the MMU alias translation now refuses core0 accesses until the relevant bus bit is cleared.
- 2026-04-16: Direct qtests now cover the active `esp-hal`-style contract for flash/PSRAM alias accesses: MMU mapping alone is not sufficient, and the guest must clear the relevant `CTRL1` shut bit before the mapped window becomes usable.
- 2026-04-16: SPI1 flash program and sector erase operations now refresh any mapped flash pages in the EXTMEM mirror, so a guest that parks the other core and mutates flash through SPI1 sees the updated contents through the cache alias without forcing an MMU remap.
- 2026-04-16: A board-path functional regression now exercises the full `FlashStorage::multicore_auto_park()` contract from a guest ROM: the APP core runs a shared-DRAM counter, the PRO core parks it through RTC_CNTL stall-magic, mutates flash via SPI1 WREN+PP and WREN+SE while the counter is frozen, verifies the ICACHE alias reflects each mutation, then unparks the APP core and confirms its counter resumes before semihosted exit.
- 2026-04-16: `EXTMEM_DCACHE_FREEZE` and `EXTMEM_ICACHE_FREEZE` now participate in the deferred-completion framework. `FREEZE_DONE` no longer asserts instantly on enable — it defers until the matching domain's in-flight SYNC/PRELOAD/AUTOLOAD ops drain (re-armed against `CACHE_STATE` idle), and clearing `FREEZE_ENA` releases the suspend in the same write. A direct qtest pins the cross-ordering: a DCACHE FREEZE issued while a DCACHE SYNC is in flight waits for the SYNC drain, while an ICACHE FREEZE issued in parallel completes independently because cross-domain ops do not gate each other.
- 2026-04-16: DBUS MMU rewrites touching PSRAM-typed entries now emit an `info_report` when DCACHE is not suspended. Hardware does not expose a fault signal for this case, so enforcement stays observability-only — the log surfaces guests that skip the ROM `Cache_Suspend_DCache()` prologue without inventing a divergent hardware bit. This is documented as a known divergence in `ESP32S3_EMULATION_GAPS.md`.
- 2026-04-16: Remaining work in this track narrows to the still-open `CORE0/1_ACS_CACHE_INT_*` residual reject surface if a guest starts reading those bits, plus any ROM-internal per-line suspend ownership behavior if it becomes guest-visible.

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

## Errors

| Error | Attempt | Resolution |
| --- | --- | --- |
| Removing clock CPU references broke `esp32s3_clock_propagate_rates()` because those references are still needed for CPU clock updates. | `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test` after converting SYSTEM core1 RUNSTALL to a GPIO output. | Restored `ESP32S3ClockState.cpu[]` for clock-rate propagation only, kept RUNSTALL as a separate named GPIO into the SoC run-state owner, rebuilt successfully, and reran targeted/full verification. |
