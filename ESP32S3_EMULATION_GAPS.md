# ESP32-S3 Accuracy Doc And Immediate Fidelity Program

## Overview

This document tracks the known fidelity gaps in the `esp32s3` machine model in this fork and the current follow-on program for closing the highest-value gaps for the active T-Deck Pro workload.

The immediate peripheral, deferred-foundation, core-SoC, first T-Deck prioritization plan, the 2026-04-18 follow-on clock/reset/sleep cleanup plan, the 2026-04-18 reset-fanout phase, the 2026-04-18 RTC time-trigger phase, the 2026-04-18 RTC light-sleep configuration phase, and the 2026-04-18 RTC SDIO config phase are historical records under `docs/plans/implemented/`. There is no separate active `docs/plans/in-progress/` ESP32-S3 fidelity plan right now; the remaining guest-triggered queues are tracked directly in this document.

The current scope is intentionally limited to the remaining high-risk follow-on work:

- broader sleep and clock realism beyond the qtest-pinned RTC, SYSTIMER, TIMG, and UART contract
- reset-tree fidelity beyond the newly owned digital reset surface
- trigger-based cache/MMU and Xtensa follow-ons only when guest evidence demands them

Broader silicon-completeness work such as analog PLL dynamics, full cache microarchitecture realism, a full ESP32-S3-specific EMAC replacement, and speculative Xtensa configured-core/TIE coverage remain source-gated and out of scope unless the active workload or stronger references promote them.

## Current State Snapshot

As of 2026-04-03, the target is materially stronger than a "boots-only" model, but it is still not a high-fidelity general-purpose ESP32-S3 hardware surrogate.

- The immediate stage-1 peripheral correctness program is complete and the focused `esp32s3` qtest suite is green.
- The current board path is in much better shape than before: SPI1, GP-SPI, routed GPIO/IO_MUX, USB Serial/JTAG, I2C, UART, PMS, RNG, and SHA now have direct regression coverage for the currently exercised surface.
- The recent EPD black-screen regression was traced to incorrect GDMA descriptor-address reconstruction. The runtime path now uses ESP32-S3 DMA RAM addressing semantics, the simulator renders again, and the qtests were aligned to the corrected hardware contract.
- The target is best understood as "board-path complete for the active firmware path, selectively modeled elsewhere". Many subsystems still behave functionally rather than faithfully.
- The active light-sleep path now freezes SYSTIMER in hardware and leaves elapsed-time compensation to the adjacent firmware wake path, matching the current T-Deck contract more closely than the earlier always-counting model.

## Core SoC Fidelity Guardrails (2026-04-15)

The completed core-SoC fidelity plan is tracked at `docs/plans/implemented/esp32s3-core-soc-fidelity/plan.md`. That plan was information-gated: every implementation task declared whether it targeted register accuracy, SDK-contract accuracy, board-path accuracy, a compatibility shim, or was blocked for accuracy.

Green focused tests are required but not sufficient evidence of silicon fidelity. The focused `esp32s3` qtest suite and board-level cache reject functional test prove the modeled contracts covered by those tests. They do not prove full ESP32-S3 hardware accuracy for analog PLL behavior, complete power-domain sequencing, cache cycle timing, physical RNG behavior, or undocumented Xtensa/TIE details.

### Reference Availability

Local reference checks for Phase 0:

- Present: ESP-IDF source tree at `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf`.
- Present by dependency lock: adjacent T-Deck Pro firmware references `esp-rs/esp-hal` git dependencies in `Cargo.toml` and `Cargo.lock`, including `esp-hal`, `esp-storage`, `esp-radio`, `esp-rtos`, and `esp-pacs` for ESP32-S3.
- Not found under `external-resources`: ESP32-S3 Technical Reference Manual files using expected `esp32s3`, `trm`, `technical reference`, or `reference manual` filename patterns.
- Not found under `external-resources`: Xtensa ISA/manual/reference files using expected `xtensa`, `isa`, `manual`, or `reference` filename patterns.
- Later source check: converted TRM material exists outside `external-resources` at `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm`, and Xtensa PDF/converted material exists at `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/xtensa`. Use these local corpora for TRM/ISA-backed work when a phase needs manual evidence, while preserving the earlier finding that matching manual files were not found under `external-resources`.

TRM-dependent and ISA-manual-dependent work is source-gated on locating the exact relevant section in the local converted/PDF corpora or adding a stronger source for that task. ESP-IDF, ESP HAL, ESP IRS packages, Rust SDK/PAC sources, active firmware behavior, and hardware probes can still support bounded register, SDK-contract, or board-path models.

### Evidence Matrix

| Subsystem | Current source classes | Local source anchors | Target fidelity | Accuracy limit |
| --- | --- | --- | --- | --- |
| Generic MMIO burn-down | ESP-IDF register headers, Rust SDK/PAC register definitions, firmware traces, qtests | `components/soc/esp32s3/register/soc/*.h`, adjacent `Cargo.lock` esp-pacs entries | Register-accurate for identified offsets; RAZ/WI or compatibility shim for unsupported offsets | Full behavior of unmodeled devices is blocked until each owning block has source-backed semantics |
| APB_CTRL date/revision | ESP-IDF SoC register headers and boot/runtime revision checks | `components/soc/esp32s3/register/soc/apb_ctrl_reg.h`, `apb_ctrl_struct.h` | Register-accurate for date/revision and explicit QEMU-origin compatibility value | Broader APB_CTRL behavior remains source-blocked without TRM coverage or firmware need |
| ANA / PLL ready | ESP-IDF clock/regi2c programming sequences, firmware polling behavior | `components/esp_hal_clock/esp32s3`, `components/esp_hal_regi2c/esp32s3`, `components/soc/esp32s3/register/soc/rtc_i2c_reg.h` | Compatibility shim for firmware-visible ready bits | Analog PLL calibration, lock timing, jitter, and failure modes are blocked for accuracy |
| RTC, reset, sleep, wake | ESP-IDF PMU/RTC HAL, low-power support, Rust SDK sleep usage, active firmware paths | `components/esp_hal_pmu/esp32s3`, `components/esp_hal_rtc_timer/esp32s3`, `components/esp_hw_support/lowpower/port/esp32s3`, adjacent firmware sleep code | SDK-contract accurate and board-path accurate for modeled wake/reset flows | Full power-domain retention, brownout behavior, and analog reset sequencing are blocked for accuracy |
| Cache, MMU, flash, PSRAM | ESP-IDF ROM cache patches, SPI flash code, MMU support, Rust `esp-storage`, active firmware storage paths | `components/esp_rom/patches`, `components/esp_mm/port/esp32s3`, `components/spi_flash/esp32s3`, adjacent `esp-storage` dependency | SDK-contract accurate for MMU, fault, reject, flash, and PSRAM setup contracts | Cache cycle timing, line replacement, bus contention, and flash-controller micro-timing are blocked for accuracy |
| Clock tree | ESP-IDF clock HAL, SoC system registers, RTC clock users, qtests | `components/esp_hal_clock/esp32s3`, `components/soc/esp32s3/register/soc/system_reg.h`, `rtc_cntl_reg.h` | SDK-contract accurate for supported sources, dividers, and consumers | Analog PLL dynamics, source-switch latency, jitter, and undocumented divider interactions are blocked for accuracy |
| Interrupt matrix | ESP-IDF interrupt register headers, Xtensa interrupt support, qtests | `components/soc/esp32s3/register/soc/interrupt_core0_reg.h`, `interrupt_core1_reg.h`, `components/xtensa` | Register-accurate for mapping, status windows, and output routing | Reserved-source suppression must be classified as source-backed hardware behavior or QEMU compatibility policy |
| eFuse | ESP-IDF eFuse tables, fields, utility code, SoC eFuse registers | `components/efuse/esp32s3`, `components/soc/esp32s3/register/soc/efuse_reg.h`, `efuse_struct.h` | Register-accurate for synthetic contents and protection mechanics | Factory personalization and security-sensitive provisioning are synthetic unless supplied through an explicit eFuse image |
| PMS | ESP-IDF sensitive/world-controller register headers, SDK usage, qtests | `components/soc/esp32s3/register/soc/sensitive_reg.h`, `world_controller_reg.h` | Compatibility shim or register-accurate for source-backed offsets | Full internal permission/security policy is blocked unless source-backed or guest-observed |
| RNG | ESP-IDF SoC register headers, SDK usage, qtests | `components/soc/esp32s3/include/soc/wdev_reg.h`, `soc_caps.h` | Compatibility shim using host entropy for the documented data path | Physical entropy source behavior, conditioning, and statistical hardware properties are blocked for accuracy |
| Xtensa backend | ESP-IDF Xtensa support, QEMU Xtensa backend, current guest binaries, TCG tests | `components/xtensa`, `components/xtensa/esp32s3`, `target/xtensa`, `tests/tcg/xtensa` | Base ISA or guest-failure-driven accuracy | Broad ESP32-S3 TIE/SIMD completeness is blocked unless exact configured-core/TIE references are supplied |

### Required Verification Floor

Every future implementation phase that changes this core-SoC surface must keep the following checks passing unless the phase explicitly documents a concrete blocker:

- `QTEST_QEMU_BINARY=build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_sleep_wake.py`

The Xtensa atomctl TCG regression is registered by `tests/tcg/xtensa/Makefile.softmmu-target` through the normal Xtensa softmmu wildcard test list. On this machine, the local build tree does not expose a Ninja TCG test target and GNU make is unavailable, so backend-related phases must run the atomctl regression through the documented local fallback path until a full TCG harness runner is available.

### Final Core-SoC Fidelity Status

| Subsystem | Final status | Boundary |
| --- | --- | --- |
| Generic MMIO burn-down | Register-accurate for replaced SPI0/SPI_MEM, ASSIST_DEBUG, APB_SARADC, and SENS active offsets; compatibility shim for remaining out-of-scope catch-all windows | Remaining LEDC and radio/internal catch-all hits are deferred to their owning peripheral/radio phases; APB_SARADC/SENS are register-surface shims only, not analog behavior models |
| APB_CTRL date/revision | Register-accurate for ESP32-S3 date; compatibility shim for QEMU-origin and legacy ECO marker words | Broader APB_CTRL/SYSCON behavior remains source-gated to future owning phases |
| ANA / PLL ready | Compatibility shim | Analog PLL calibration, lock timing, jitter, and failure modes remain blocked for accuracy |
| RTC, reset, sleep, wake | SDK-contract accurate and board-path accurate for modeled light-sleep wake/reject, guest timer wake recovery, and reset-domain flows | Full power-domain sequencing, retention timing, brownout interactions, ULP/touch/USB wake, and analog reset behavior remain blocked for accuracy |
| Cache, MMU, flash, PSRAM | SDK-contract accurate for MMU entries, flash/PSRAM mappings, invalid-entry faults, per-core rejects, and cache-maintenance busy/done/cancel behavior | Cache cycle timing, replacement policy, contention, pipeline stalls, and flash-controller micro-timing remain blocked for accuracy |
| Clock tree | SDK-contract accurate for XTAL, RC_FAST, PLL-selected CPU rates, APB derivation, RTC/SYSTEM-driven source updates, UART timing, and TIMG APB/XTAL counter rates | Analog PLL dynamics, oscillator startup, jitter, DFS/source-switch latency, SYSTIMER sleep compensation, and broader peripheral fanout remain blocked for accuracy |
| Interrupt matrix | Register-accurate for source mapping, per-core status windows, output routing, remap behavior, and documented reserved-source suppression policy | The reserved-source suppression itself is a QEMU compatibility policy, not proven hardware behavior |
| eFuse | Register-accurate for the synthetic ESP32-S3 eFuse image, programming flow, reset state, and protection mechanics covered by ESP-IDF tables | Factory personalization, security provisioning, coding-error repair behavior, and physical burn timing remain synthetic or blocked |
| PMS | Register-surface accurate for source-backed SENSITIVE/PMS offsets; compatibility shim for unsupported offsets | Lock-bit enforcement and real permission denial side effects remain blocked for accuracy |
| RNG | Compatibility shim for the documented `WDEV_RND_REG` data path using host entropy | Physical entropy source behavior, conditioning, startup timing, and statistical hardware properties remain blocked for accuracy |
| Xtensa backend | Base backend gate covered for the current guest path and `S32C1I`/`ATOMCTL` local-memory distinction | Broad ESP32-S3 configured-core, TIE, and SIMD completeness remains blocked unless exact extension references or reproduced opcode failures are supplied |

### Phase 1 Generic MMIO Status

The catch-all ESP32-S3 MMIO path is now observable through QEMU trace events:

- `esp32s3_unimplemented_io_read`: physical address, access size, returned value, and guest PC.
- `esp32s3_unimplemented_io_write`: physical address, access size, written value, and guest PC.

The qtest `/xtensa/esp32s3/generic-mmio/compatibility-storage` pins the remaining compatibility behavior. Stored readback is now restricted to explicitly classified out-of-scope LEDC and FE/FE2/NRX/BB radio/internal windows, with adjacent words remaining independent. Other unmapped fallthrough addresses such as `DR_REG_WCL_BASE + 0xf0` are deterministic read-as-zero/write-ignore while still emitting the trace events above. This is not a hardware-fidelity claim. It is a regression guard so future replacement of catch-all offsets with explicit models is deliberate and reviewable.

Phase 1 closure used guest/firmware traces with these events enabled to classify active catch-all hits by owner and source availability. Boot-critical SPI0/SPI_MEM hits and core-debug ASSIST_DEBUG hits were replaced with explicit narrow models. Active catch-all use after Phase 1 is restricted to analog/peripheral/radio/internal windows outside this core-SoC pass; any future trace hit in an in-scope core boot, reset, sleep, cache, clock, interrupt, eFuse, PMS, RNG, or Xtensa-backend path should be treated as a new fidelity bug rather than accepted as generic storage behavior.

The first copied-flash firmware trace on 2026-04-16 produced 289 generic-MMIO trace lines in 15 seconds. The dominant active region was `DR_REG_SPI0_BASE` / SPI_MEM at `0x60003000`, led by 86 reads of `SPI_MEM_FSM_REG(0)`. SPI0 is now mapped as an explicit SPI_MEM register bank and the active SPI_MEM configuration offsets are stored in `hw/ssi/esp32s3_spi.c`. A second 15-second trace dropped the generic-MMIO total to 80 lines and removed all `0x60003000` SPI0 hits.

The same pass also moved `DR_REG_ASSIST_DEBUG_BASE` out of the generic region. The narrow shim stores `RCD_PDEBUGENABLE` and `RCD_RECORDING` and returns zero for read-only `RCD_PDEBUGPC`. A third 15-second trace dropped the generic-MMIO total to 75 lines and removed all `0x600ce000` ASSIST_DEBUG hits.

Remaining active catch-all hits from the final Phase 1 trace are classified as:

| Region | Active offsets | Source state | Phase 1 classification |
| --- | --- | --- | --- |
| `DR_REG_APB_SARADC_BASE` | `0x00`, `0x04`, `0x18`, `0x28`, `0x38`, `0x3c`, `0x70` | ESP-IDF `apb_saradc_reg.h` present | Resolved by explicit narrow APB_SARADC register-surface shim; ADC conversion, calibration, arbitration timing, and analog behavior remain unmodeled |
| `DR_REG_SENS_BASE` | `0x10`, `0x34`, `0x3c` | ESP-IDF `sens_reg.h` present | Resolved by explicit narrow SENS register-surface shim; sensor muxing, SAR measurement behavior, and analog power sequencing remain unmodeled |
| `DR_REG_LEDC_BASE` | `0x00`, `0x04`, `0x08`, `0x0c`, `0xa0`, `0xd0` | ESP-IDF `ledc_reg.h` present | Peripheral PWM surface; defer to peripheral pass unless board-path timing depends on it |
| `DR_REG_ASSIST_DEBUG_BASE` | `0x48`, `0x4c`, `0x5c` | ESP-IDF `assist_debug_reg.h` present | Resolved by explicit narrow core-debug shim |
| FE2, FE, NRX, BB radio/internal windows | `0x600050f0`, `0x60006090`, `0x6001ccd4`, `0x6001d054` | No public local register header found | Blocked for accuracy without exact radio-internal references; out of current core-SoC scope |

The first follow-on P1 slice moves APB_SARADC and SENS out of the catch-all path. The modeled APB_SARADC surface covers offsets `0x00`, `0x04`, `0x18`, `0x28`, `0x38`, `0x3c`, and `0x70` with ESP-IDF-backed reset defaults and writable masks. The modeled SENS surface covers offsets `0x10`, `0x34`, and `0x3c` with source-backed writable masks. Unsupported offsets in both windows are deterministic read-as-zero/write-ignore rather than stored readback. This prevents generic-MMIO echo behavior from hiding missing analog-control modeling, but it does not claim ADC data-path fidelity.

The second follow-on P1 slice removes broad stored-readback behavior from the fallback itself. Only deferred LEDC and blocked radio/internal ranges keep compatibility storage. Any other unmodeled core-SoC fallthrough is now RAZ/WI, so new active hits remain visible through trace logs and tests instead of being masked by generic echo state.

### Phase 2 APB_CTRL And ANA Status

`DR_REG_APB_CTRL_BASE` / `DR_REG_SYSCON_BASE` now has an explicit narrow model instead of a RAM-backed register island. The supported surface is:

- `+0x3fc`: ESP32-S3 `APB_CTRL_DATE_REG` / `SYSCON_DATE_REG`, returning the ESP-IDF reset value `0x02101150`.
- `+0x3f8`: QEMU-origin compatibility word, returning `0x51454d55` (`QEMU`).
- `+0x07c`: legacy ESP32-derived ECO3 marker, returning `0x96042000` only as a compatibility bridge for older guests that inherited the pre-S3 offset.

Writes to the modeled APB_CTRL words are ignored and unsupported APB_CTRL offsets are read-as-zero/write-ignore. This is a deliberate narrow contract, not a full APB_CTRL/SYSCON implementation. Clock-tree, retention, memory-policy, and security-policy fields remain source-gated to their owning phases.

The ANA PLL-ready bit remains a compatibility shim. QEMU returns the firmware-visible ready bit at ANA offset `0x40`, but analog PLL calibration, lock timing, jitter, and failure modes are blocked for accuracy without a hardware probe or exact vendor analog reference.

### Phase 3 RTC Sleep/Wake Register Status

The RTC_CNTL fallback store was removed for the sleep/wake register surface. QEMU now exposes explicit fields for the ESP-IDF and local sleep-document contract used by timer, GPIO, EXT1, watchdog-write-protect, pad-hold, date/version, and the adjacent firmware's light-sleep configuration flows:

- Supported RTC_CNTL offsets: `0x000`, `0x004`, `0x008`, `0x00c`, `0x010`, `0x014`, `0x018`, `0x020`, `0x038`, `0x03c`, `0x044`, `0x04c`, `0x050`-`0x05c`, `0x064`, `0x068`, `0x074`, `0x07c`, `0x080`, `0x084`, `0x088`, `0x08c`, `0x090`, `0x0b0`, `0x0b4`, `0x0b8`, `0x0bc`, `0x0c0`-`0x0cc`, `0x0d8`, `0x0dc`, `0x0e0`, `0x130`, and `0x1fc`.
- Source-backed write masks are enforced for `OPTIONS0`, `TIMER2`, `SLP_TIMER1`, `WAKEUP_STATE`, `EXT_WAKEUP_CONF`, `SLP_REJECT_CONF`, `SDIO_CONF`, `RTC`, `PWC`, `BIAS_CONF`, `REGULATOR_DRV_CTRL`, `DIG_PWC`, `SWD_CONF`, `PAD_HOLD`, `EXT_WAKEUP1`, and `DATE`. Write-only bits such as `MAIN_TIMER_ALARM_EN`, `SWD_FEED`, and `EXT_WAKEUP1_STATUS_CLR` no longer appear as generic readback.
- `RTC_CNTL_SDIO_CONF` at `0x07c` now has ESP-IDF-backed reset defaults and write masks for the active firmware light-sleep path. QEMU models the documented digital register contract only; SDIO regulator analog behavior, current-limit timing, and power-rail sequencing remain out of scope.
- `RTC_CNTL_STATE0` now reports explicit QEMU sleep states: awake, sleep-requested, sleeping, rejected, and woke. The qtest suite covers timer wake, GPIO low wake, GPIO reject, EXT1 low wake, EXT1 high wake, immediate EXT1 wake, stale hardware-bit writes, and reject-cause clear.
- Source-known but unmodeled RTC_CNTL offsets are deterministic unsupported behavior: reads return zero and writes are ignored. This includes `0x01c`, `0x024`-`0x034`, `0x040`, `0x048`, `0x060`, `0x06c`-`0x070`, `0x078`, `0x094`-`0x0ac`, `0x0d0`-`0x0d4`, `0x0e4`-`0x12c`, `0x134`-`0x154`, and any undefined gap up to `0x1f8`. The qtest suite pins `RETENTION_CTRL` at `0x140` as the unsupported-register sentinel for this RTC pass.

This is SDK-contract and board-path accurate for the modeled light-sleep wake/reject flows. It is not a full ESP32-S3 power manager. Full internal power-domain sequencing, CPU-retention DMA timing, retention-memory save/restore, brownout interactions, analog reset behavior, RTC watchdog escalation timing, ULP/touch/USB wake behavior, EXT1 status latching, and oscillator/settling delays remain blocked for accuracy without more detailed source evidence or hardware probes.

### P0 Light-Sleep Board Transition Status

The first follow-on P0 sleep/clock/reset slice adds an explicit board-visible light-sleep transition instead of keeping RTC sleep as internal bookkeeping only:

- `RTC_CNTL` emits a named `light-sleep` output only while the modeled RTC state machine is in `ESP32S3_RTC_SLEEP_SLEEPING`.
- The ESP32-S3 SoC consumes that output and pauses/resumes modeled CPUs through one centralized run-state owner.
- The same owner also combines RTC per-core CPU-stall requests and SYSTEM core1 RUNSTALL, so clearing one hold source does not resume a CPU that is still held by another modeled source.
- SYSTEM core1 RUNSTALL is now exposed from the clock block as a named GPIO output instead of directly calling `cpu_pause()` / `cpu_resume()` inside the clock device.
- Direct qtests cover timer sleep asserting `light-sleep` until wake, GPIO sleep rejection never asserting `light-sleep`, RTC CPU-stall output behavior, and SYSTEM core1 RUNSTALL output behavior.
- The functional test `test_xtensa_esp32s3_sleep_wake.py` boots a ROM ELF through the ESP32-S3 board `-bios` path, programs RTC timer wake from guest code, enters light sleep, resumes after the modeled wake, and validates guest-visible wake status before semihosting exit.

This is a QEMU-visible SoC transition contract and a guest-level recovery regression, not hardware proof of full ESP32-S3 low-power behavior. The local sleep reference still identifies broader missing behavior: light sleep should switch away from PLL before entry, stop XTAL/PLL and usually RC_FAST, compensate stopped SYSTIMER time on wake, model power-domain and peripheral gating decisions, and handle CPU retention/power-down variants. Those remain blocked for accuracy until each item is backed by the local TRM/ESP-IDF/esp-hal evidence or a hardware probe.

### Phase 3 Reset Domain Status

The ESP32-S3 reset path is now split into named helpers for PROCPU reset, APPCPU reset, owned digital-peripheral reset, digital reset, and full-chip reset. This makes the current QEMU contract explicit:

- PROCPU and APPCPU resets reset only the targeted modeled CPU, select the RTC-configured static-vector mode, clear CPU watchpoints, and restore `CPENABLE` as a QEMU compatibility bridge for system-mode Xtensa execution.
- Digital reset resets both modeled CPUs plus the currently owned digital-peripheral subset: interrupt matrix, UARTs, GPIO, RNG, I2C, SPI0/SPI1, EXTMEM cache/MMU, SYSTEM clock block, GDMA, SHA/AES/RSA/HMAC/DS/PMS/XTS_AES, TIMG0/TIMG1, SYSTIMER, USB Serial/JTAG, IO_MUX, GPSPI, and the explicit analog register shims already owned by the SoC helper.
- After software digital reset resets the SYSTEM clock block, the SoC reapplies the retained RTC clock selection into the modeled system clock tree so guest-visible software reset remains aligned with RTC-owned clock bookkeeping.
- Full-chip reset cold-resets RTC_CNTL first, then runs the same owned digital reset helper and CPU resets. This keeps host/QMP `system_reset` aligned with the existing full-chip clock defaults while still clearing the widened digital surface.
- Guest software reset bits in `RTC_CNTL_OPTIONS0` now dispatch the modeled ESP32-S3 reset domain locally instead of manufacturing a QEMU process-level reset event. Host/QMP `system_reset` remains wired through QEMU's reset framework and enters the explicit full-chip reset helper.

The qtest suite now pins guest-visible reset cause, PROCPU/APPCPU reset isolation from dirty digital state, software digital reset clearing representative clock/cache/GPIO/IOMUX/GPSPI/SHA/PMS/TIMG state, host/QMP full-chip reset restoring the widened digital surface to host-reset defaults, and RTC scratch retention across software CPU/digital resets. Full ESP32-S3 reset-tree fidelity remains incomplete for analog rails, brownout, LP-domain sequencing, retention timing, and peripherals that are still outside the currently owned digital subset.

### Phase 4 Cache, MMU, Flash, And PSRAM Status

The supported EXTMEM cache/MMU contract is intentionally functional and source-bounded:

- MMU entries use the ESP32-S3 64 KB page format modeled by ESP-IDF and the current QEMU header: page number bits `[13:0]`, invalid bit `14`, and type bit `15` selecting flash or PSRAM. Reserved bits are forced to zero on writes, and the table remains readable through the MMU register window.
- Invalid DCache and ICache translations latch `EXTMEM_CACHE_ILG_INT_ST.MMU_ENTRY_FAULT_ST` plus `EXTMEM_CACHE_MMU_FAULT_CONTENT` and `EXTMEM_CACHE_MMU_FAULT_VADDR`; clearing the illegal-cache interrupt clears the latched fault metadata.
- Flash-backed mappings are read-only IOMMU mappings into the flash mirror. A write through a flash-backed DCache or ICache alias is rejected and records per-core DBUS/IBUS reject status, reject virtual address, access attribute, tag attribute, and interrupt status. Direct qtest MMIO can prove core0 reject metadata; core1 attribution depends on `current_cpu` and remains covered by the board-level functional cache-reject test.
- SPI1 page program and erase commands now refresh affected flash-backed MMU pages in the EXTMEM mirror, so mapped flash aliases reflect guest-visible flash mutations without requiring a synthetic MMU remap. This contract is now pinned at both layers: a direct MMIO qtest (`/xtensa/esp32s3/cache/flash-spi-updates-mapped-alias`) and a board-path functional regression (`tests/functional/test_xtensa_esp32s3_multicore_park_flash.py`) that parks the APP core through RTC_CNTL stall-magic before mutating flash, matching `FlashStorage::multicore_auto_park()` usage.
- PSRAM-backed mappings are read/write IOMMU mappings into the attached PSRAM model when the `esp32s3` machine has nonzero `-m` memory. Without PSRAM, a PSRAM-typed MMU entry gives no translation permission.
- `EXTMEM_DCACHE_CTRL1` and `EXTMEM_ICACHE_CTRL1` now reset with both per-core bus-shut bits asserted, matching ESP-IDF `extmem_reg.h`. MMU alias translation now honors those bits for the current accessing core, so a mapped flash or PSRAM window remains inaccessible until the guest clears the relevant core bus-shut bit.
- DCache and ICache sync, preload, and autoload control registers now have per-operation deferred completion deadlines. Starting an operation clears the read-only done bit, `CACHE_STATE` reports the matching domain busy until that operation's own deadline, overlapping operations complete in deadline order, and clearing an enable bit before the deadline cancels that operation without manufacturing completion.
- DCache and ICache freeze controls now model the ROM `Cache_Suspend_DCache()` / `Cache_Resume_DCache()` contract at SDK granularity. Enabling `EXTMEM_DCACHE_FREEZE.DCACHE_FREEZE_ENA` (or the ICache equivalent) defers `FREEZE_DONE` until the matching domain drains any in-flight SYNC, PRELOAD, or AUTOLOAD operation, so guest polling through `Cache_Wait_Idle(0)` on `EXTMEM_CACHE_STATE` observes the same ordering as silicon. Clearing the enable bit releases the latched suspend and drops `FREEZE_DONE` in the same write. No line-ownership, cache pipeline stall, or bus-arbitration behavior is modeled beyond this busy/idle drain handshake.
- DBUS MMU rewrites that touch a PSRAM-typed entry while `DCACHE_FREEZE_ENA` is not set emit a `qemu_log_mask(LOG_GUEST_ERROR, ...)` log line so the contract violation is visible; hardware does not expose a fault signal for this case, so enforcement is observability-only. The channel is silent by default and can be enabled with `-d guest_error` (add `-D file` to route it to a log file). Current `esp-hal` PSRAM bring-up legitimately hits this on every boot because it walks a contiguous DBUS MMU range without the ROM `Cache_Suspend_DCache()` prologue; the log lets you inspect which entries it touched without inventing a divergent hardware fault.

This is SDK-contract accurate for the modeled MMU programming, flash/PSRAM mapping, per-core bus gating, invalid-entry fault, reject, cache-maintenance completion, and suspend/resume ordering semantics needed by the current firmware and local SDK sources. It is not cycle-accurate cache hardware. Cache cycle timing, line fill/eviction/replacement policy, dirty-line writeback ordering, per-line suspend ownership, bus contention, pipeline stall timing, flash-controller micro-timing, encryption throughput timing, and the full cache PMS/access-mask matrix remain blocked for accuracy without hardware probes or exact vendor microarchitectural references.

### Phase 5 Clock Tree And Interrupt Matrix Status

The supported SYSTEM clock contract is narrow and SDK-contract oriented:

- CPU clock source selection follows the ESP-IDF `SOC_CPU_CLK_SRC_*` values exposed through `SYSTEM_SYSCLK_CONF.SOC_CLK_SEL`: XTAL uses the modeled XTAL frequency, RC_FAST uses the ESP-IDF approximate `17.5 MHz`, and PLL selection uses the modeled `SYSTEM_CPU_PER_CONF.CPUPERIOD_SEL` values currently needed by the guest path.
- APB frequency is derived as `min(cpu_hz, 80 MHz)`, matching the ESP-IDF clock-tree rule that APB/AHB is fixed at 80 MHz when CPU clock is sourced from PLL and otherwise follows lower CPU sources.
- RTC writes to `RTC_CNTL_CLK_CONF.SOC_CLK_SEL` continue to propagate into `SYSTEM_SYSCLK_CONF` and the supported consumers. The qtest suite pins RTC-driven XTAL selection and UART timing changes.
- The supported modeled consumers are Xtensa CPU clock inputs, UART timing/autobaud behavior, and ESP32-S3 timer-group APB/XTAL counter and watchdog rates. Other APB users are not yet connected to the clock model and must not be treated as source-sensitive.
- `SYSTEM_CPU_PER_CONF` and `SYSTEM_SYSCLK_CONF` now mask guest writes to the source-backed fields present in ESP-IDF `system_reg.h`. Peripheral clock-enable/reset registers and `SYSTEM_CLOCK_GATE` are explicit deterministic register state, but individual peripheral clock gating and reset side effects are still owned by their device-specific phases.
- SYSTEM clock changes now emit a SoC-level clock update so direct SYSTEM writes and RTC-driven clock source changes update TIMG consumers consistently. The qtest suite pins TIMG APB counter timing at 80 ticks/us under PLL/APB=80 MHz and 40 ticks/us after switching SOC clock to XTAL/APB=40 MHz.

The interrupt matrix contract is also narrow but explicit:

- Mapping registers reset to the ESP-IDF documented value `16`, store the low 5 bits on writes, and are exposed for both CPU windows using the documented `0x800` per-core stride.
- Status windows at `+0x18c` through `+0x198` and `+0x98c` through `+0x998` reflect the currently asserted peripheral interrupt source levels.
- Sources `15`, `23`, `33`, `34`, and `46` are suppressed as a QEMU compatibility policy because the current Rust/PAC interrupt vector table has null entries there; status reads mask those bits so firmware does not dispatch through address zero.
- Routing to CPU0 and CPU1 uses the Xtensa configured external interrupt table. The direct qtest suite proves mapping readback, status assert/deassert, per-core status windows, and reserved-source suppression. Existing device tests continue to prove interrupt-source assertions through the matrix input side, and the board-level cache-reject functional test proves CPU-visible routed interrupt delivery on both cores for the currently exercised reject sources.

This phase does not model analog PLL lock, oscillator startup, jitter, DFS/source-switch latency, divider settling, peripheral clock-domain crossing outside TIMG, SYSTIMER stop/compensation across sleep, or undocumented divider interactions. Those are blocked for accuracy without stronger timing references or hardware probes.

### Phase 6 eFuse, PMS, And RNG Status

The eFuse contract is now explicit for the synthetic S3 eFuse image used by QEMU:

- ESP32-S3 reset defaults and write masks are source-backed from ESP-IDF `efuse_reg.h` for `EFUSE_CLK`, `EFUSE_CONF`, `EFUSE_DAC_CONF`, `EFUSE_RD_TIM_CONF`, `EFUSE_WR_TIM_CONF1`, `EFUSE_WR_TIM_CONF2`, and `EFUSE_DATE`.
- The generic recent-ESP eFuse model now resets transient program/check registers, command state, interrupt state, and ready status deterministically on reset.
- Programming preserves one-way eFuse semantics by OR-ing new bits into the persistent mirror. Guest-visible read registers update after an eFuse read command, matching the ESP-IDF post-program flow.
- Write protection is tested for user-data block programming via the ESP32-S3 `WR_DIS.BLOCK_USR_DATA` bit. Read protection is tested for key block visibility via the `RD_DIS.BLOCK_KEY0` bit.
- The ESP32-S3 subclass now bounds key access to hardware key blocks 0 through 5. Block 10 remains `SYS_DATA_PART2`, not a seventh key block.

The PMS contract is a register-surface contract only:

- Source-backed SENSITIVE/PMS offsets used by the current model have reset values and write masks from ESP-IDF `sensitive_reg.h`, including cache access, MMU access, DMA APB permission pairs through the current immediate surface, `SENSITIVE_CLOCK_GATE`, `SENSITIVE_RTC_PMS`, and `SENSITIVE_DATE`.
- Unsupported PMS offsets are read-as-zero/write-ignore. Lock-bit enforcement and real memory-permission denial side effects are not modeled in this phase.
- This is not a security-policy model. It prevents broad raw echo behavior and pins the SDK-visible register surface, but it does not enforce the full internal permission matrix.

The RNG contract remains intentionally compatibility-oriented:

- Only `WDEV_RND_REG` at `0x6003507c` is modeled as the 32-bit data path from ESP-IDF `wdev_reg.h`.
- Reads return host-backed random values. Writes are ignored so guest mistakes do not crash QEMU.
- Unsupported offsets remain zero. The tests require repeated reads to be non-stable, but they do not claim hardware entropy quality, conditioning, startup behavior, or statistical equivalence.

Factory-programmed eFuse values, MAC/chip personalization, security provisioning, eFuse coding-error repair behavior, physical eFuse burn voltage/timing, PMS enforcement, and physical RNG behavior remain synthetic or blocked for accuracy unless an explicit eFuse image, hardware probe, or stronger vendor reference is added.

### Phase 7 Xtensa Backend Verification Gate

`tests/tcg/xtensa/test_s32c1i_atomctl.S` is now the explicit guard for the ESP32-S3 `S32C1I`/`ATOMCTL` local-memory distinction that affects broader firmware coverage. It is part of the normal Xtensa softmmu TCG test set because `tests/tcg/xtensa/Makefile.softmmu-target` wildcard-registers usable `*.S` tests. No `Makefile.target` registration change was needed.

Local execution of the normal TCG harness remains environment-gated on this machine because no Ninja TCG target is exposed in the current build tree and `gmake` is not installed. The regression was therefore verified through the project-documented fallback path: build `linker.ld`, `crt.o`, `vectors.o`, and `test_s32c1i_atomctl` with the installed Xtensa ESP32-S3 cross-compiler, then run the resulting ELF under `build/qemu-system-xtensa -M sim -cpu esp32s3 -semihosting -icount 6`. That path exits successfully and is sufficient for this phase's backend gate.

Future Xtensa backend fixes must follow these accuracy rules:

- Base Xtensa ISA fixes may use the local Xtensa ISA manual under `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/xtensa`, provided the exact instruction semantics are cited in the implementation or test rationale.
- ESP32-S3 configured-core, TIE, or SIMD changes require either an exact ESP32-S3 configured-extension reference or a reproduced guest opcode failure with a bounded expected behavior. Broad translation expansion from guesswork is blocked for accuracy.
- `target/xtensa/translate_tie_esp32s3.c` must not grow proactive instruction support unless the instruction is backed by exact semantics or by a failing guest binary whose intended behavior can be independently established.
- Every backend fix needs proof at the correct level: an existing TCG test, a new focused TCG test, or a minimal board ROM probe when the behavior cannot be isolated in the TCG harness.
- Broad ESP32-S3 extension completeness remains blocked for accuracy until the exact ESP32-S3 configured-core and TIE option references are supplied.

### Current Fidelity / Risk Table

| Subsystem | Current state | Gap vs real hardware | Likely real-usage risk |
| --- | --- | --- | --- |
| Boot and active board path | Good enough for the current firmware path and focused regressions | Still relies on selective modeling rather than full-chip behavior | Medium |
| GP-SPI + GDMA | Good enough for current EPD, SD, LoRa, and qtest paths | Timing, busy windows, and broader DMA sequencing are still simplified | Medium |
| GPIO matrix + IO_MUX | Board-path complete for the routed signals in active use | Not a full silicon-complete routing model | Medium |
| Generic MMIO surface | Instrumented and narrower than before, but still present for out-of-scope windows | Unknown registers can still appear to work via stored readback unless traced and replaced with explicit models | High |
| Clock / reset / sleep | Explicit narrow SDK contract for CPU/APB/RTC-selected clock propagation, TIMG APB/XTAL rate fanout, modeled RTC/reset flows, and board-visible light-sleep CPU hold transitions | PLL dynamics, source-switch latency, SYSTIMER sleep compensation, power-domain/peripheral gating, deep power behavior, and wider peripheral clock fanout remain incomplete | High |
| Cache / MMU / external memory | Functional SDK-contract model for MMU, flash, PSRAM, faults, rejects, and maintenance-operation completion | Still not cycle-accurate; no line replacement, contention, stall timing, or flash-controller micro-timing | Medium to High |
| USB Serial/JTAG, I2C, UART, SHA | Board-path complete with RX, completion semantics, clock-derived timing, and IRQ coverage | DMA error paths, uncommon timing modes, and multi-CPU interrupt routing remain incomplete | Medium |
| eFuse / PMS / RNG | Explicit synthetic eFuse contract, source-backed PMS register masks, host-backed RNG data path | Factory personalization, security provisioning, PMS enforcement, and physical entropy behavior remain synthetic or blocked | Low to Medium |
| Ethernet | Generic `open_eth` stand-in with direct link/MII and descriptor loopback regression coverage | Still not an ESP32-S3-specific EMAC model or full PHY implementation | Low for the current workload, Medium to High if future firmware depends on deeper EMAC details |
| RMT | Unimplemented | Entire block still absent | High if firmware depends on it |
| Xtensa backend | Current guest path plus the `S32C1I`/`ATOMCTL` local-memory distinction are covered by focused regression gates | Architectural edge cases and broad configured-core/TIE/SIMD completeness remain blocked without exact extension references or reproduced opcode failures | High for broader firmware coverage |

## Current Gaps

- Unimplemented MMIO is now much less likely to be papered over accidentally. The remaining catch-all register window stores writes and echoes reads only for explicitly classified out-of-scope LEDC and FE/FE2/NRX/BB radio/internal ranges; other unmapped fallthrough addresses are RAZ/WI while still traceable through `esp32s3_unimplemented_io_read` and `esp32s3_unimplemented_io_write`. Phase 1 classified the active catch-all hits and replaced the boot-critical SPI0/SPI_MEM and core-debug ASSIST_DEBUG hits with explicit narrow models, Phase 2 replaced the APB_CTRL RAM island with an explicit narrow model, and P1 follow-on work replaced APB_SARADC/SENS active offsets with source-backed register-surface shims before limiting fallback storage to the deferred out-of-scope ranges. The ANA PLL-ready behavior remains a named compatibility shim that forces firmware-visible ready bits high so polling loops keep moving. RMT remains explicitly mapped as an unimplemented device.
- GP-SPI now reads chip-select and DC signals through the routed-signal layer (`esp32s3_gpio_get_routed_signal_level`) rather than hard-coded GPIO numbers. However, the default signal-to-pin assignments (EPD_CS→GPIO34, EPD_DC→GPIO35, SD_CS→GPIO48, LoRa_CS→GPIO3) are still hardwired in GPIO reset state rather than being derived from firmware-written routing registers.
- SPI2/SPI3 were intentionally a minimum-function stub. Transfers complete instantly, `USR` clears immediately, and `TRANS_DONE` is raised right away instead of following a more realistic controller state progression.
- Clock, reset, and sleep/power behavior are only partially modeled. The `SYSTEM` block handles a small subset of registers, RTC sleep/wake logic is tailored to current timer/GPIO/EXT1 paths, and reset now uses local guest software reset dispatch but still lacks full silicon reset-tree, analog, and retention sequencing.
- Most previously placeholder peripherals have been tightened to board-path-complete behavior: USB Serial/JTAG now supports RX delivery, FIFO-used status reporting, and RX/TX interrupt state; I2C clears and sets DONE bits per-command with deferred completion IRQ delivery; UART derives baud and pulse timing from the active clock configuration (falling back to 40 MHz only when no clock device is linked); PMS now has an explicit source-backed register table for the modeled surface and RAZ/WI behavior elsewhere; SHA asserts and clears interrupt state on completion for both DMA and non-DMA paths; RNG is intentionally narrow with one 32-bit host-backed data register and deterministic ignored writes. Remaining gaps include DMA error paths, uncommon timing modes, unsupported I2C slave/APB-nonfifo modes, and multi-CPU interrupt routing.
- External memory and cache behavior are functional rather than cycle-accurate. Flash-backed and PSRAM-backed MMU remaps are immediate, invalid translations latch fault metadata, flash writes latch reject metadata, and cache sync/preload/autoload requests now complete through per-operation deferred deadlines that drive `CACHE_STATE` busy/idle reporting. Timing, replacement, contention, and pipeline effects are deliberately not modeled.
- Some blocks are substituted with generic IP rather than an S3-specific model. Ethernet still uses `open_eth`, but the current tree now makes the QEMU-visible contract explicit: `MIICOMMAND`/MII link polling stays latched correctly, backend link toggles update `MIISTATUS`, and loopback mode can drive descriptor RX from TX for direct regression coverage.
- SPI1 had an outright correctness bug in the flash transfer loop: the byte loop compared the payload value instead of the loop index, making the transfer path data-dependent. (Now fixed; see Stage 2.)
- The generic Xtensa backend still has known accuracy gaps such as remaining reject-surface coverage gaps and unimplemented opcode paths.
- Real `esp32s3` board guest probing is no longer blocked by ROM handoff. The board now loads custom ROM ELFs through `-bios` into each CPU address space correctly, and the tree now has checked-in functional regressions for recoverable cache-alias reject handling on both CPU0 and CPU1 plus guest RTC timer light-sleep wake recovery. The remaining cache-specific gap is only the broader reject surface beyond the active board path.

### Clock/Reset/Sleep Follow-On Inventory (2026-04-18)

Background commits reviewed for the current board-control path:

- `163978001f` introduced the RTC sleep-state register surface and state fields.
- `0f1ef9a87b` added the timer/GPIO light-sleep state machine.
- `367f037177` wired Core 1 `RUNSTALL` and the RTC CPU-stall magic-value path.
- `3e30797dca` connected GPIO wakeup notifications into the SoC board path.
- `e1da504cc2` added EXT1 light-sleep wakeup handling.
- `a7d95fc530` made RTC clock-source writes update the modeled SoC clock state.
- `767ea45d46` added direct qtests for timer wakeup, reset transitions, and CPU stall.

The original 2026-04-04 shortcut inventory is now historical. The archived P0 slices closed several of those items: guest software reset now dispatches through the local SoC reset path, light sleep is a board-visible CPU-hold transition instead of internal bookkeeping, and TIMG consumers now follow modeled APB/XTAL clock changes.

The remaining follow-on items in this area are:

- The RTC timer surface now models the source-backed `TIME_UPDATE` trigger bits for system stall, XTAL-off/light-sleep transitions, and software reset completion, and exposes both current and previous trigger captures through `RTC_TIME_LOW0/HIGH0` and `RTC_TIME_LOW1/HIGH1`. Broader oscillator stop/start timing and power-domain sequencing are still incomplete.
- The adjacent firmware's RTC light-sleep configuration registers now have explicit masks and reset defaults for the source-backed `OPTIONS0`, `TIMER2`, `RTC`, `PWC`, `BIAS_CONF`, `REGULATOR_DRV_CTRL`, and `DIG_PWC` surface. The ambiguous RTC clock-gating and SDIO fields remain deferred until their ownership is reconciled against the existing RTC-to-system clock contract.
- The active light-sleep path now freezes SYSTIMER and resumes it on wake, while the adjacent firmware remains responsible for compensating elapsed RTC time after wake. Broader oscillator stop/start timing and peripheral-gating realism remain incomplete.
- Digital-reset fanout now covers the currently owned digital subset with direct regression coverage. Broader reset-tree sequencing, LP/analog domains, and any future owned peripherals remain deferred until a guest or source-backed need appears.
- The RTC-to-clock handoff now has one explicit owner: the RTC block updates only its local clock-selection bookkeeping, and the SoC callback remains the single place that applies those selections into the modeled system clock tree.
- Clock and sleep fanout are still narrow compared to real hardware. After auditing the active guest, the only dynamic QEMU-visible consumers still proven today are TIMG and UART, so wider peripheral fanout remains deferred until a concrete guest dependency appears. Oscillator stop/start timing and peripheral/power-domain gating are still incomplete.
- Full-chip RTC reset now restores the explicitly modeled RTC_CNTL register surface to reset defaults instead of only rebasing time. Wider domain-aware retention timing, analog/power sequencing, and broader reset fanout remain incomplete.

### Task 2 Cache/MMU Dependency Inventory (2026-04-04)

Background commits reviewed for the current cache/MMU path:

- `81f8593bc5` introduced the ESP32-S3 cache/MMU model, including MMU entry storage, flash page fill, and IOMMU-backed translation.
- `e0bfd3961f` moved the cache block onto the newer three-stage reset path but did not materially broaden the guest-visible cache/MMU sequence.

Current firmware and library code rely on a narrower subset of cache/MMU behavior than the full `EXTMEM` register surface suggests:

- Boot and OTA code rely on the live MMU table contents being readable at `0x600C5000` with the ESP32-S3 `64 KB` page format. `esp-hal-ota` determines the currently running partition by taking a function pointer, deriving the MMU entry index from the executing virtual address, checking the invalid bit, and reconstructing the backing flash address from the table entry.
- Boot-time PSRAM bring-up relies on DBUS MMU programming rather than only on the static flash mapping. `esp-hal` scans the MMU table for the last mapped flash page, suspends DCache, calls `cache_dbus_mmu_set(...)` to install SPIRAM mappings, clears the DBUS shut bits in `EXTMEM_DCACHE_CTRL1`, and resumes DCache before the allocator starts placing large buffers in PSRAM.
- Runtime flash services rely on the ROM flash operations, not on a higher-level filesystem abstraction alone. `esp-storage` issues `esp_rom_spiflash_read`, `unlock`, `erase_sector`, and `write`, so the emulator needs working flash-backed translation and MMU-backed visibility during both boot and later filesystem or OTA traffic.
- Multi-core flash writes and erases rely on explicit Core 1 parking around the ROM flash calls. The active firmware uses `FlashStorage::multicore_auto_park()` for littlefs and BLE OTA, so the guest-visible contract we care about first is "park the other core, perform the flash op, then unpark" rather than a cycle-accurate cache-disable implementation. The board-path functional regression `tests/functional/test_xtensa_esp32s3_multicore_park_flash.py` now exercises this full contract end-to-end: APP-core counter loop, RTC_CNTL stall-magic park, SPI1 WREN+PP and WREN+SE with frozen counter, EXTMEM ICACHE-alias coherence, and APP-core counter resume after unpark.
- Panic-path crashdump writes rely on the same raw flash path while assuming the other core is already stalled. That means the important emulation surface is still the flash/MMU path itself, even when the multi-core helper is intentionally bypassed.
- The current reviewed firmware does not appear to rely on the broader EXTMEM management surface such as cache prelock/lock controls, preload/autoload sequencing, PMS reject capture, wraparound control, or cache/MMU fault reporting. Those registers exist in the header today, but they are not part of the confirmed dependency set for the active T-Deck Pro workload.
- There is now direct ESP32-S3 qtest coverage for flash-backed MMU remapping, SPI1 flash mutation visibility through mapped aliases, PSRAM-backed MMU mapping, the `CTRL1` state and per-core bus gating touched by PSRAM bring-up, invalid-MMU fault latching, core0 DBUS/IBUS reject metadata, per-operation sync/preload/autoload completion ordering, clear-while-busy cancellation, and fault preservation across cache-maintenance operations. Remaining cache/MMU simplifications are now mostly in the "leave cycle-accuracy for later" bucket: simplified freeze semantics, no cache-line replacement model, no contention/stall timing, and no ROM-internal cache-disable depth.

### Task 3 EMAC Inventory And Current Contract (2026-04-05)

Background commits reviewed for the current Ethernet path:

- `7591824ec4` introduced the ESP32-S3 machine and wired a generic `open_eth` NIC directly into the board at `DR_REG_EMAC_BASE`, its descriptor window at `+0x400`, and `ETS_ETH_MAC_INTR_SOURCE`.
- No later board-specific EMAC commit replaced that wiring. The current tree still instantiates `open_eth` directly from `hw/xtensa/esp32s3.c`.

Current firmware and board-path findings:

- The adjacent T-Deck Pro firmware repo does not contain direct EMAC, Ethernet, RMII, or PHY bring-up code in `src/`. Networking is expressed through `esp_radio::wifi` plus `embassy-net`, not through an Ethernet MAC driver.
- `Cargo.toml` enables `esp-radio` with the `wifi` feature and `embassy-net`, and the runtime path calls `esp_radio::wifi::new(...)` before spawning an `embassy_net::Runner` over `esp_radio::wifi::Interface`.
- There is no app-level evidence that the current firmware reads or writes the ESP32-S3 EMAC register block, configures an external PHY, or expects RMII link state from the board.
- The QEMU board path is correspondingly generic rather than board-specific. It instantiates `open_eth`, maps its two MMIO windows, and routes its interrupt, but it does not model a T-Deck-specific Ethernet PHY or any exercised board wiring around that block.
- The exact EMAC behavior the current firmware touches is therefore effectively none. `open_eth` is a latent fidelity risk for future Ethernet-aware guests, not an actively exercised dependency of the current Wi-Fi-based T-Deck Pro workload.
- Local ESP-IDF context matters here: the bundled `components/esp_eth/src/openeth/esp_eth_mac_openeth.c` driver is already a QEMU-only OpenCores path, so the smallest correct slice is not replacing `open_eth` but making that path explicit and testable.
- Current scope decision: keep `open_eth` for the present board path, but tighten its visible contract instead of leaving it as an untested dormant placeholder.
- Current launch contract: the supported host-backed backend for the `esp32s3` board EMAC path is `-nic user,id=emac0,model=open_eth`. QEMU defaults may still provide a matching NIC automatically, but explicit no-NIC launches such as `-nic none` now stay bootable while warning that `open_eth` was not instantiated and networking is disabled.
- Current tree: `open_eth` now latches `MIICOMMAND`, preserves `SCANSTAT`-driven link polling behavior across host reads, updates `MIISTATUS.LINKFAIL` when the backend link changes, and supports descriptor TX-to-RX loopback when `MODER.LOOPBCK` is enabled.
- Current coverage: the ESP32-S3 qtest suite now boots the board with `-nic user,id=emac0,model=open_eth`, uses QMP `set_link` to verify MII-visible link up/down transitions, and proves TX/RX descriptor plus IRQ behavior through loopback.
- This remains an Ethernet-only QEMU surface. There is still no ESP32-S3 radio or UART-backed Wi-Fi device model in this fork, and the firmware-side transport selection stays outside this repo.
- Remaining future risk: the board still does not model an ESP32-S3-specific EMAC block or a realistic external PHY beyond the OpenCores/QEMU path. That replacement stays deferred until a guest actually needs more than the current QEMU-targeted contract.

### Task 4 Xtensa Backend Queue (2026-04-04)

The active Xtensa/backend queue now lives in `docs/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md` so it stays separate from the peripheral backlog and tied to current firmware behavior.

Current ranking after the T-Deck Pro re-prioritization:

- `P2` watch work: remaining ESP32-S3 reject surface beyond the current qtest and board-regression matrix. The shared illegal-cache path and the first `CORE0/1` reject path now exist, and the tree has checked-in board regressions for both the single-core/core0 path and the SMP/core1 path, but the rest of the reject/write-IC/access-mask matrix is only partially modeled. Keep this gated behind a concrete guest dependency.
- `P2` trigger work: remaining missing Xtensa opcodes beyond the current ESP32-S3 core/FPU/TIE coverage. The generic unimplemented-opcode fallback still exists, but the current guest stress instructions (`ee.movi.32.a`, `ee.zero.accx`, `ee.vmulas.s16.accx`) are already translated, so no active firmware blocker is confirmed there yet.

Recently resolved in this track:

- `HELPER(check_atomctl)` now skips `ATOMCTL` cache/PIF gating for accesses that resolve to local DataRAM, matching the `s32c1i` local-memory contract used by ESP32-class Xtensa cores.
- Xtensa softmmu coverage now includes an `esp32s3`-specific `test_s32c1i_atomctl` regression that proves `ATOMCTL=0` still faults on backed sysram (`0x6000_0100`) while succeeding on local DataRAM.
- The `esp32s3` board path now has checked-in functional cache-reject regressions in `tests/functional/test_xtensa_esp32s3_cache_reject.py`, proving that ROM ELFs loaded via `-bios` reach recoverable cache-alias reject handlers on both core0 and core1.
- The Xtensa TCG linker script now places the reset stub at `XCHAL_RESET_VECTOR0_VADDR`, which was required for `esp32`/`esp32s3` softmmu guests to boot on the generic `sim` machine at all.
- The ESP32-S3 cache model now latches `EXTMEM_CACHE_ILG_INT_ST` and `EXTMEM_CACHE_MMU_FAULT_{CONTENT,VADDR}` on invalid MMU accesses, and the board routes the resulting illegal-cache IRQ to `ETS_CACHE_IA_INTR_SOURCE` with direct qtest coverage.
- The ESP32-S3 cache model now also latches the first per-core reject metadata and IRQ path: flash-backed write rejects populate `CORE0/1_ACS_CACHE_INT_ST` plus the matching `CORE0/1_{DBUS,IBUS}_REJECT_{ST,VADDR}` registers, the board routes those lines to `ETS_CACHE_CORE0/1_ACS_INTR_SOURCE`, and the qtest suite covers both core0 DBUS and core0 IBUS reject assert/clear contracts.
- The board path now also tolerates one-core softmmu runs. `esp32s3 -smp 1` no longer crashes in `esp32s3_soc_reset()` by touching unrealized CPU1 state, and the qtest suite has a direct single-CPU boot smoke test to hold that line.

### Current Risk Notes

- The highest remaining accuracy risk is no longer the active display path. The bigger remaining problems are the deferred foundation items: clock/reset/sleep fidelity, cache/MMU sequencing, generic-MMIO dependence, deeper EMAC fidelity beyond the current OpenCores contract, and broader Xtensa accuracy.
- Performance-sensitive guest code is likely to diverge from hardware because cache and MMU operations are modeled as functional state transitions rather than realistic completion and contention behavior.
- Firmware that touches only the currently exercised board path is much more likely to work than firmware that depends on deep power-management, uncommon DMA/peripheral corner cases, EMAC behavior beyond the current OpenCores contract, RMT, or architectural edge conditions.

### Recently Resolved

- **GDMA descriptor-address reconstruction**: The EPD black-screen regression was caused by incorrect DMA RAM buffer-address handling in descriptor reads. Fixed to use ESP32-S3 DMA RAM addressing semantics. A new non-destructive `esp_gdma_read_channel_data` function avoids modifying descriptor owner bits or channel link state during reads.
- **SHA DMA path**: `esp_sha_continue_dma` reads GDMA OUT channel data and runs SHA compress blocks. DMA-backed SHA operations are now functional for the exercised command paths.
- **GP-SPI two-phase transfer model**: Transfers now use a two-phase timer (800 ns execute + 1200 ns completion) with up to 4 retries and a clock-only fallback, replacing the old instant-complete model. A `transfer_prefers_fifo` heuristic avoids re-reading stale DMA descriptors for small FIFO-only transfers.

## Prioritized Backlog

### High Impact

- `P0` [DONE] Fix the SPI1 transfer-loop bug so TX/RX bounds use the loop index instead of the payload byte value.
- `P1` [DONE] Replace GP-SPI hard-coded board pin reads with a routed-signal layer backed by GPIO routing state and a minimal IO_MUX model for the current board path.
- `P1` [PHASE 1 DONE] Replace boot-critical generic-MMIO behavior with explicit narrow models for the firmware-touched SPI0/SPI_MEM and ASSIST_DEBUG offsets; remaining active catch-all hits are classified as out-of-scope analog/peripheral/radio surfaces for this core-SoC pass.
- `P1` [DONE] Make GP-SPI completion semantics more faithful than an unconditional immediate `USR` clear plus `TRANS_DONE`.

### Easy Wins

- `P2` [DONE] Add USB Serial/JTAG RX support, FIFO-ready/backpressure semantics, and interrupt updates.
- `P2` [DONE] Tighten I2C completion semantics, DONE-bit handling, ACK error behavior, and explicit rejection of unsupported modes.
- `P2` [DONE] Make UART pulse timing and baud calculation derive from active clock state instead of fixed 40 MHz assumptions.
- `P2` [DONE] Replace PMS raw echo behavior with reset/default/read-as-zero-write-ignore behavior for the currently touched surface.
- `P2` [DONE] Keep RNG host-backed, but restrict it to a small modeled data path instead of broad undefined behavior.
- `P2` [DONE] Wire SHA completion into realistic interrupt assert/clear behavior for existing command paths.

### Deferred

- `P3` Deep clock/reset/sleep fidelity rework to eliminate QEMU-only identity/workaround behavior.
- `P3` [PHASE 4 DONE] Cache/MMU functional sequencing for MMU entries, flash, PSRAM, invalid-entry faults, per-core reject metadata, and cache-maintenance busy/done state; cycle timing and microarchitectural realism remain blocked.
- `P3` Replace `open_eth` with a more ESP32-S3-specific EMAC model if firmware needs more than the current QEMU OpenCores link / packet contract.
- `P3` Broader Xtensa architectural fidelity work once guest firmware depends on those features.

## Immediate Program

### Stage 0: Doc

- Keep this file at repo root as the fork-local source of truth for the ESP32-S3 fidelity program.

### Stage 1: Regression Harness

- Add ESP32-S3-focused qtests rather than depending on large guest-firmware integration tests.
- Cover the first wave with direct MMIO- and device-level tests for:
  - SPI1 transfer correctness
  - GP-SPI completion/IRQ behavior
  - GPIO status and routing register behavior
  - USB Serial/JTAG TX/RX/FIFO state
  - I2C completion and ACK error signaling
  - UART baud/pulse behavior under different clock configurations
  - SHA IRQ assert/clear behavior
  - boot-critical MMIO offsets moved out of the generic echo region

### Stage 2: High-Impact Correctness Fix

- Fix SPI1 TX/RX loop bounds.
- Add a regression that uses payload values both below and above the transfer length so the old byte-vs-index bug would fail deterministically.
- Status:
  - Done: SPI1 TX/RX loop bounds fixed; regression test covers payload values both below and above transfer length.

### Stage 3: IO_MUX + GPIO Matrix Routing

- Add an explicit minimal IO_MUX device instead of leaving the block unimplemented.
- Add one routed-signal layer so GP-SPI consumes routed board-control signals instead of hard-coded GPIO literals.
- Keep this first slice board-path-complete rather than silicon-complete: support the routed signals used by the current EPD, SD, and LoRa path and leave unrelated matrix functionality for later work.
- Status:
  - Done: IO_MUX device, routed signal plumbing for EPD/SD/LoRa control pins, richer staged GP-SPI transfer semantics, and qtest coverage for default + non-default remap state.

### Stage 4: Reduce Generic-MMIO Dependence

- Remove boot-critical PLL/ANA progress behavior from the generic MMIO echo region.
- Keep the fallback region only for genuinely unmodeled addresses that are outside the immediately supported firmware path.
- Status:
  - Done: boot-critical ANA `PLL_DONE` bit (offset 0x40, bit 24) handled as a special case in the `esp32s3_ana_ops` memory region. The surrounding ANA register space at 0x6000e000 remains an echo store with no further special-case handling.

### Stage 5: Easy Wins

- USB Serial/JTAG:
  - add RX delivery from the attached chardev
  - report FIFO space based on actual buffered state
  - raise and clear interrupt state for RX/TX completion paths
- I2C:
  - stop forcing DONE bits on reads
  - clear/set DONE only when command state actually changes
  - preserve deferred completion IRQ delivery
  - keep unsupported slave/APB-nonfifo modes explicit
- UART:
  - compute baud and pulse timing from the current clock configuration
  - keep autobaud deterministic but clock-derived
- PMS:
  - stop treating the whole block as a raw echo register file
  - return reset/default or RAZ/WI behavior for the immediate surface
- RNG:
  - keep host entropy as the source
  - keep the visible register surface intentionally narrow
- SHA:
  - assert interrupt state when operations complete
  - make clear/enable semantics match the existing synchronous completion path
- Status:
  - Done: USB Serial/JTAG RX/FIFO-ready path, I2C DONE semantics, clock-derived UART timing, narrow RNG/PMS behaviors, SHA completion IRQ handling, and GP-SPI completion staging.

## Deferred Follow-Ups

### Stage 6: Deferred Foundation

- Expand routed-pin fidelity beyond the current board path.
- Model GP-SPI transfer timing, busy windows, and DMA completion with more realistic sequencing.
- Replace additional generic-MMIO dependencies as firmware begins to touch them.
- Revisit clock tree, cache/MMU, EMAC, and Xtensa backend fidelity once higher-priority peripheral behavior is stable.

### Stage 6 Status

- Captured in the implemented deferred-foundation plan at `docs/plans/implemented/esp32s3-deferred-foundation/plan.md`.

## Source References

- `hw/xtensa/esp32s3.c`
- `hw/gpio/esp32s3_gpio.c`
- `hw/gpio/esp32s3_iomux.c`
- `hw/ssi/esp32s3_gpspi.c`
- `hw/ssi/esp32s3_spi.c`
- `hw/misc/esp32c3_jtag.c`
- `hw/i2c/esp32_i2c.c`
- `hw/char/esp32_uart.c`
- `hw/misc/esp32s3_pms.c`
- `hw/misc/esp32s3_rng.c`
- `hw/misc/esp_sha.c`
- `hw/xtensa/esp32s3_clk.c`
- `hw/misc/esp32s3_cache.c`
- `target/xtensa/op_helper.c`
- `target/xtensa/translate.c`
