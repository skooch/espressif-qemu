# ESP32-S3 Core SoC Fidelity Plan

## Goal
Turn the remaining ESP32-S3 core-SoC quality recommendations into information-gated implementation tracks that improve QEMU accuracy without claiming fidelity beyond the available public and local reference material.

## Current Phase
Phase 8

## Scope
This plan covers core SoC behavior only: generic MMIO removal, ANA/APB boot shims, RTC/reset/sleep, cache/MMU, clocking, interrupt matrix, eFuse, PMS/RNG, Xtensa backend confidence, and documentation. It does not plan unrelated peripheral fidelity.

## Fidelity Levels
- `register-accurate`: register addresses, reset values, access widths, read/write masks, interrupt status, clear behavior, and documented side effects match the reference material.
- `SDK-contract accurate`: behavior matches ESP-IDF, ESP HAL, ESP IRS packages, the official Rust SDK, and active firmware expectations, while hidden silicon behavior remains out of scope.
- `board-path accurate`: behavior is correct for the current T-Deck Pro firmware path and tested at the board or ROM-probe level.
- `compatibility shim`: behavior is intentionally synthetic and documented as QEMU compatibility rather than ESP32-S3 hardware fidelity.
- `blocked for accuracy`: implementation is deferred until a hardware probe, exact vendor document, or failing guest trace supplies the missing facts.

## Evidence Rules
- Use local ESP-IDF register headers, HAL LL code, ROM patch code, and examples before changing a device model.
- Use ESP HAL, ESP IRS packages, the official Rust SDK, and active firmware behavior to define `SDK-contract accurate` behavior.
- Use the ESP32-S3 Technical Reference Manual and Xtensa ISA manual only after their local paths are verified.
- Treat analog PLL internals, full power-domain sequencing, cache cycle timing, physical RNG behavior, and undocumented Xtensa/TIE details as unproven unless a source explicitly defines them.
- For every ambiguous behavior, either add a hardware-probe requirement or document the resulting model as a compatibility shim.

## File Map
- Modify: `ESP32S3_EMULATION_GAPS.md` (fidelity limits, evidence matrix, verification floor, and updated status)
- Modify: `docs/plans/in-progress/esp32s3-core-soc-fidelity/plan.md` (tracked progress)
- Create: `docs/plans/in-progress/esp32s3-core-soc-fidelity/progress.md` (session log)
- Modify: `hw/xtensa/esp32s3.c` (generic MMIO, APB_CTRL, ANA mapping, reset glue, board wiring)
- Modify: `hw/ssi/esp32s3_spi.c` (explicit SPI0/SPI_MEM register-surface handling for boot-critical active offsets)
- Modify: `include/hw/ssi/esp32s3_spi.h` (SPI_MEM register declarations and state for active SPI0 offsets)
- Modify: `hw/xtensa/esp32s3_clk.c` (SYSTEM clock contract and CPU/APB clock propagation)
- Modify: `include/hw/xtensa/esp32s3_clk.h` (clock state needed by explicit contract)
- Modify: `hw/xtensa/esp32s3_intc.c` (interrupt matrix routing, status, reserved-source policy)
- Modify: `include/hw/xtensa/esp32s3_intc.h` (state or constants required by interrupt tests)
- Modify: `hw/misc/esp32s3_rtc_cntl.c` (RTC sleep/wake/reset model and fallback removal)
- Modify: `include/hw/misc/esp32s3_rtc_cntl.h` (RTC state and register definitions)
- Modify: `hw/gpio/esp32s3_gpio.c` (RTC wake integration only where required by core wake behavior)
- Modify: `include/hw/gpio/esp32s3_gpio.h` (GPIO-to-RTC wake interface only where required)
- Modify: `hw/misc/esp32s3_cache.c` (cache/MMU state, fault, reject, and operation sequencing)
- Modify: `include/hw/misc/esp32s3_cache.h` (cache register layout and helper declarations)
- Modify: `hw/nvram/esp32s3_efuse.c` (ESP32-S3-specific eFuse subclass behavior)
- Modify: `hw/nvram/esp_efuse.c` (generic eFuse behavior only when shared mechanisms are corrected)
- Modify: `include/hw/nvram/esp32s3_efuse.h` (ESP32-S3 eFuse class/state declarations)
- Modify: `include/hw/nvram/esp_efuse.h` (generic eFuse state and register declarations)
- Modify: `hw/misc/esp32s3_pms.c` (explicit PMS compatibility surface)
- Modify: `include/hw/misc/esp32s3_pms.h` (PMS register declarations)
- Modify: `hw/misc/esp32s3_rng.c` (explicit RNG compatibility surface)
- Modify: `include/hw/misc/esp32s3_rng.h` (RNG contract comments or state only if required)
- Modify: `target/xtensa/core-esp32s3.c` (ESP32-S3 core option declarations only when source-backed)
- Modify: `target/xtensa/op_helper.c` (ISA helper behavior only when source-backed)
- Modify: `target/xtensa/translate_tie_esp32s3.c` (TIE translation only for confirmed guest or manual-backed instructions)
- Modify: `tests/qtest/esp32s3-test.c` (direct core-SoC regression coverage)
- Modify: `tests/functional/test_xtensa_esp32s3_cache_reject.py` (board-level cache reject probes if widened)
- Modify: `tests/tcg/xtensa/test_s32c1i_atomctl.S` (Xtensa atomctl coverage if integration changes are needed)
- Modify: `tests/tcg/xtensa/Makefile.softmmu-target` (TCG test registration if missing from build)
- Modify: `CLAUDE.md` (document new required verification or command failures if discovered)
- Create: `docs/plans/in-progress/esp32s3-core-soc-fidelity/findings.md` (trace classification and information-gating findings)

## Phases

### Phase 0: Evidence Baseline And Guardrails
- [x] Create an evidence matrix in `ESP32S3_EMULATION_GAPS.md` mapping each subsystem in this plan to the exact reference class used: ESP-IDF, ESP HAL, ESP IRS packages, Rust SDK, ESP32-S3 TRM, Xtensa ISA manual, firmware trace, or hardware probe.
- [x] Confirm whether local ESP32-S3 TRM and Xtensa ISA manual files are present under `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources`; record the absence in `ESP32S3_EMULATION_GAPS.md` and mark TRM/ISA-dependent work as source-blocked until those files are supplied.
- [x] Add a fidelity-level note to `ESP32S3_EMULATION_GAPS.md` stating that green qtests prove focused regression behavior, not full ESP32-S3 silicon fidelity.
- [x] Define the verification floor in `ESP32S3_EMULATION_GAPS.md`: `QTEST_QEMU_BINARY=build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test` and the ESP32-S3 cache reject functional test must pass after every implementation phase.
- **Status:** complete

### Phase 1: Generic MMIO Burn-Down
- [x] Instrument `hw/xtensa/esp32s3.c` so every active-path hit to `esp32s3_io_ops` can be collected with address, access size, access type, and guest PC when QEMU is launched with a debug flag or trace event.
- [x] Add a qtest in `tests/qtest/esp32s3-test.c` proving that known unsupported generic-MMIO offsets do not silently gain hardware semantics beyond the explicit compatibility behavior selected for this phase.
- [x] Replace the boot-critical SPI0/SPI_MEM generic-MMIO hits identified by firmware trace with an explicit narrow register-surface model using ESP-IDF `spi_mem_reg.h` offsets and existing QEMU SPI_MEM behavior.
- [x] Replace the core-debug ASSIST_DEBUG generic-MMIO hits identified by firmware trace with an explicit narrow shim for the source-backed registers the guest touches.
- [x] Decide whether the remaining APB_SARADC and SENS active hits are in scope for this core-SoC pass or should stay documented as analog/peripheral compatibility surfaces for a later peripheral pass.
- [x] Keep `esp32s3_io_ops` only for out-of-scope regions and document every remaining active-path hit in `ESP32S3_EMULATION_GAPS.md`.
- **Status:** complete

### Phase 2: APB_CTRL And ANA Shim Separation
- [x] Replace the RAM-backed APB_CTRL date/revision hack in `hw/xtensa/esp32s3.c` with an explicit APB_CTRL register model that implements documented date/revision reads and rejects unsupported writes deterministically.
- [x] Add qtests in `tests/qtest/esp32s3-test.c` for APB_CTRL reset value, read-only behavior, revision value, and QEMU-origin compatibility value if the compatibility value remains required.
- [x] Split the ANA PLL-ready behavior in `hw/xtensa/esp32s3.c` into a named compatibility shim with comments stating that analog PLL calibration, lock timing, and failure modes are not modeled from current sources.
- [x] Add an `ESP32S3_EMULATION_GAPS.md` entry marking ANA/PLL internals as `blocked for accuracy` unless a hardware probe or exact vendor analog reference is added.
- **Status:** complete

### Phase 3: RTC, Reset, Sleep, And Wake State Model
- [x] Replace each RTC register fallback in `hw/misc/esp32s3_rtc_cntl.c` that is touched by ESP-IDF, ESP HAL, the Rust SDK, or active firmware with explicit register behavior and a qtest covering reset value, write mask, read value, and side effect.
- [x] Keep untouched RTC registers as deterministic unsupported behavior and list those offsets in `ESP32S3_EMULATION_GAPS.md` as outside the supported RTC contract.
- [x] Refactor RTC sleep state in `hw/misc/esp32s3_rtc_cntl.c` into explicit states for awake, sleep-requested, sleeping, rejected, and woke, with tests for timer wake, GPIO wake, EXT1 low wake, EXT1 high wake, immediate wake, and reject.
- [x] Replace reset glue in `hw/xtensa/esp32s3.c` with helper functions for PROCPU reset, APPCPU reset, digital reset, peripheral reset, and full-chip reset; each helper must state which QEMU process-level behavior remains a compatibility bridge.
- [x] Add qtests in `tests/qtest/esp32s3-test.c` for guest-visible reset cause, CPU reset side effects, peripheral reset side effects covered by the current model, and preservation or clearing of RTC scratch state.
- [x] Mark full internal power-domain sequencing, retention timing, brownout interactions, and analog reset behavior as `blocked for accuracy` in `ESP32S3_EMULATION_GAPS.md` unless a hardware probe is added.
- **Status:** complete

### Phase 4: Cache, MMU, Flash, And PSRAM Contract
- [x] Define the supported cache/MMU contract in `ESP32S3_EMULATION_GAPS.md`: MMU entry programming, invalid-entry faults, per-core access rejects, sync/preload/autoload busy/done state, freeze state, flash-backed mapping, and PSRAM-backed mapping.
- [x] Improve `hw/misc/esp32s3_cache.c` so each supported cache operation has deterministic busy, done, idle, clear, and fault interaction behavior rather than only a single fixed-delay completion path.
- [x] Add qtests in `tests/qtest/esp32s3-test.c` for overlapping dcache/icache operations, clear-while-busy behavior, operation completion ordering, fault status preservation, and per-core reject metadata for every modeled reject source.
- [x] Extend `tests/functional/test_xtensa_esp32s3_cache_reject.py` only when a board-level behavior cannot be proven with direct qtest MMIO.
- [x] Mark cache cycle timing, cache line replacement, bus contention, pipeline stall timing, and flash-controller micro-timing as `blocked for accuracy` in `ESP32S3_EMULATION_GAPS.md`.
- **Status:** complete
- **Note:** No new functional-test extension was needed in Phase 4. Direct qtest MMIO now proves the register/MMU/cache-operation contract, while the existing functional cache-reject test remains the correct board-level proof for core1 reject attribution because qtest-originated accesses do not run under a core1 `current_cpu`.

### Phase 5: Clock Tree And Interrupt Matrix Contract
- [x] Define the supported clock contract in `ESP32S3_EMULATION_GAPS.md`: XTAL, RCFAST, PLL-selected CPU rates, APB derivation, RTC slow/fast clock selection, and every modeled consumer of clock updates.
- [x] Update `hw/xtensa/esp32s3_clk.c` so unsupported SYSTEM clock fields either have source-backed behavior or deterministic unsupported behavior instead of silent no-op behavior.
- [x] Add qtests in `tests/qtest/esp32s3-test.c` proving CPU clock, APB clock, RTC-derived clock update, UART timing, timer behavior, and sleep/wake timing for each supported source.
- [x] Add interrupt-matrix qtests in `tests/qtest/esp32s3-test.c` that map a source to CPU0 and CPU1, assert and deassert the source, verify status window reads, verify output IRQ lines, and verify remap behavior.
- [x] Document the reserved-source suppression policy in `hw/xtensa/esp32s3_intc.c` and `ESP32S3_EMULATION_GAPS.md` as either source-backed hardware behavior or QEMU compatibility behavior.
- [x] Mark analog PLL lock dynamics, jitter, DFS transition timing, and undocumented divider interactions as `blocked for accuracy` in `ESP32S3_EMULATION_GAPS.md`.
- **Status:** complete
- **Note:** CPU/APB clock behavior is verified through the modeled CPU-clock propagation contract and UART/APB observable timing. Timer-group and systimer source-sensitive timing are not supported modeled consumers in this phase; they are documented as blocked rather than falsely claimed. Direct qtests prove interrupt-matrix mapping/status/remap behavior, while the existing board-level cache-reject functional test remains the proof that routed matrix outputs are CPU-visible on both cores.

### Phase 6: eFuse, PMS, And RNG Explicit Contracts
- [x] Build an ESP32-S3 eFuse field map from ESP-IDF eFuse tables and register headers, then update `hw/nvram/esp32s3_efuse.c` and `include/hw/nvram/esp32s3_efuse.h` so ESP32-S3-specific behavior is not only a thin subclass.
- [x] Add qtests in `tests/qtest/esp32s3-test.c` for ESP32-S3 eFuse read, program, write-protect, read-protect, and reset behavior using synthetic eFuse contents.
- [x] Document in `ESP32S3_EMULATION_GAPS.md` that factory-programmed values, chip personalization, and security-sensitive provisioning are synthetic unless supplied through an explicit image.
- [x] Update `hw/misc/esp32s3_pms.c` so every modeled PMS register is either source-backed or explicitly RAZ/WI, and add qtests for supported and unsupported PMS offsets.
- [x] Keep `hw/misc/esp32s3_rng.c` host-backed, restrict the register surface to the documented data path, and document physical entropy behavior as a compatibility model rather than hardware-faithful RNG.
- [x] Add qtests in `tests/qtest/esp32s3-test.c` proving RNG valid read width, unsupported offset behavior, reset behavior, and non-stability across multiple reads without asserting hardware entropy quality.
- **Status:** complete
- **Note:** eFuse is register-accurate for the synthetic QEMU eFuse image and protection mechanics covered by ESP-IDF tables and direct qtests. PMS is a source-backed register-surface contract only; permission enforcement remains blocked. RNG is a host-backed compatibility model for the single documented `WDEV_RND_REG` data path, not a physical entropy model.

### Phase 7: Xtensa Backend Verification Gate
- [x] Confirm `tests/tcg/xtensa/test_s32c1i_atomctl.S` is registered by the normal Xtensa TCG verification makefile; no registration update is needed because `tests/tcg/xtensa/Makefile.softmmu-target` wildcard-adds every usable `*.S` test.
- [x] Confirm `tests/tcg/xtensa/test_s32c1i_atomctl.S` builds and runs on this machine through the documented local fallback path using the installed Xtensa ESP32-S3 cross-compiler and `build/qemu-system-xtensa -M sim -cpu esp32s3`.
- [x] Add `ESP32S3_EMULATION_GAPS.md` guidance that base Xtensa ISA fixes may use the Xtensa ISA manual, while ESP32-S3 TIE/SIMD fixes require either an exact extension reference or a reproduced guest opcode failure.
- [x] Keep `target/xtensa/translate_tie_esp32s3.c` changes gated behind a failing guest binary or exact manual-backed instruction semantics; no proactive translation expansion without one of those inputs.
- [x] Add a minimal board ROM probe or TCG test for every backend fix that cannot be proven by an existing TCG test.
- [x] Mark broad ESP32-S3 extension completeness as `blocked for accuracy` unless the exact ESP32-S3 configured-core and TIE option references are supplied.
- **Status:** complete
- **Note:** The local build tree currently does not expose a Ninja target for Xtensa TCG tests and this machine still lacks `gmake`, so Phase 7 used the repo-documented local fallback compile/run path. Registration in the normal TCG makefile is confirmed by source inspection, but full local TCG harness execution remains environment-gated until GNU make or an equivalent configured TCG runner is available.

### Phase 8: Documentation, Verification, And Plan Exit
- [ ] Update `ESP32S3_EMULATION_GAPS.md` so each subsystem has a final status: `register-accurate`, `SDK-contract accurate`, `board-path accurate`, `compatibility shim`, or `blocked for accuracy`.
- [ ] Run `QTEST_QEMU_BINARY=build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test` and record the result in the implementation progress for the active phase.
- [ ] Run `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py` and record the result in the implementation progress for the active phase.
- [ ] Run the Xtensa TCG atomctl regression using the Phase 7-confirmed registration or documented local fallback path, and record the exact command and result in the implementation progress.
- [ ] Move this plan from `docs/plans/in-progress/esp32s3-core-soc-fidelity/plan.md` to `docs/plans/implemented/esp32s3-core-soc-fidelity/plan.md` only after all required verification steps have passed or documented blockers have been accepted.
- **Status:** pending

## Decisions
| Decision | Rationale |
|----------|-----------|
| Use a peer worktree for implementation. | Repository instructions require implementation work to happen in peer worktrees under `../worktrees/`. |
| Use one parent plan with subsystem phases. | The recommendations share evidence rules, verification gates, and final documentation updates. |
| Use fidelity levels as task gates. | The available sources are strong for register and SDK behavior but weak for analog, physical RNG, power-domain internals, cache timing, and broad TIE/SIMD completeness. |
| Treat ANA PLL internals as a compatibility shim unless stronger evidence appears. | ESP-IDF and SDK sources expose software-visible polling behavior but not analog PLL calibration internals, lock dynamics, or failure modes. |
| Treat RNG as host-backed compatibility rather than hardware accuracy. | The listed sources can define the register surface and SDK usage but cannot model the physical entropy source faithfully. |
| Keep Xtensa extension expansion failure-driven or exact-manual-driven. | Base ISA material is sufficient for base ISA work; ESP32-S3 TIE/SIMD edge behavior needs exact configured-core references or concrete failing opcodes. |
| Use QEMU trace events for generic-MMIO hit collection. | Trace events are the least invasive way to collect active-path address, size, access direction, value, and guest PC without changing compatibility behavior. |
| Guard the current generic-MMIO compatibility behavior at `DR_REG_WCL_BASE + 0xf00`. | That offset is still handled by the catch-all window and provides a deterministic unsupported-address sentinel for detecting accidental semantic changes. |
| Model SPI0 as an explicit SPI_MEM register bank before broader peripheral work. | Firmware trace showed SPI0 dominated the active catch-all surface, ESP-IDF supplies register offsets, and existing QEMU SPI_MEM state can cover the active configuration registers without claiming flash timing fidelity. |
| Model ASSIST_DEBUG as a narrow core-debug shim. | Firmware trace showed only three source-backed debug-recording offsets, and removing them from the generic MMIO path reduces core-scope catch-all dependence without pulling in peripheral work. |
| Leave APB_SARADC and SENS in the documented out-of-scope catch-all set for Phase 1. | ESP-IDF headers provide register names, but the active offsets belong to analog ADC/sensor-control surfaces rather than the targeted core boot/debug surface; modeling them accurately belongs in a later analog/peripheral pass unless boot/sleep evidence proves a core dependency. |
| Treat LEDC and FE/BB/NRX trace hits as out of current core-SoC scope. | LEDC is a PWM peripheral, while FE/BB/NRX are radio/internal windows without public local register headers; neither should drive core-SoC modeling without stronger evidence. |
| Preserve the old APB_CTRL `+0x7c` ECO marker as compatibility only. | ESP32-S3 sources define APB_CTRL/SYSCON date at `+0x3fc`, but the previous model exposed the ESP32-derived ECO marker at `+0x7c`; keeping it explicitly avoids accidental guest breakage while documenting that it is not S3 hardware fidelity. |
| Make unsupported APB_CTRL offsets RAZ/WI in Phase 2. | The current source-backed requirement is date/origin compatibility only; clock, retention, memory-policy, and security-policy fields belong to later owning phases rather than broad RAM-backed semantics. |

## Errors
| Error | Attempt | Resolution |
|-------|---------|------------|
| Plan work was initially started in the main checkout. | User corrected that this must happen in a worktree. | Recorded correction in `/Users/skooch/.claude/corrections.md`, deleted the untracked plan file from the main checkout, created peer worktree `/Users/skooch/projects/tdeck-pro-rust/worktrees/esp32s3-core-soc-fidelity-phase0`, and continued there. |
| Initial worktree ESP32-S3 qtest run failed with `unknown type 'misc.esp32s3.aes'`. | Configured a fresh worktree build with only `--target-list=xtensa-softmmu`; Meson did not find Homebrew `libgcrypt`, so gcrypt-gated ESP32-S3 crypto device models were omitted. | Reconfigured with `PKG_CONFIG_PATH=/opt/homebrew/Cellar/libgcrypt/1.12.1/lib/pkgconfig`, `--enable-gcrypt`, and `--disable-gnutls`; documented the local build requirement in `CLAUDE.md`. |
| Reconfiguring with `--enable-gcrypt` and default GnuTLS failed compiling TLS sources. | Meson detected GnuTLS, but the local build failed on missing `gnutls/gnutls.h` during TLS source compilation. | Disabled GnuTLS for this ESP32-S3 verification build because the qtest and cache-reject functional verification do not require TLS. |
| Firmware-trace wrapper failed with `zsh: read-only variable: status`. | Used `status=$?` after QEMU exited. In zsh, `status` is a readonly special parameter. | Reran with `rc=$?` and kept subsequent wrappers away from zsh special parameter names. |
| Firmware-trace wrapper failed with `mktemp: mkstemp failed ... File exists`. | Used a macOS `mktemp` template with `XXXXXX` before a `.bin` or `.log` suffix. | Reran with `mktemp -t esp32s3-trace-flash` and `mktemp -t esp32s3-mmio-trace`, where macOS appends the unique suffix. |
| Targeted qtest selector matched zero tests. | Ran with `/esp32s3/spi0/mem-register-surface`, but QEMU qtest registration prefixes the target path. | Listed registered tests and reran with `/xtensa/esp32s3/spi0/mem-register-surface`. |
| Header search failed with `No such file or directory` for an ESP-IDF `register/soc/soc.h` path. | Included a guessed ESP-IDF path in an `rg` command while checking APB_CTRL base definitions. | Used the QEMU local `include/hw/misc/esp32s3_reg.h` base definition and the existing ESP-IDF `apb_ctrl_reg.h` / `syscon_reg.h` headers directly. |
| Firmware source search failed with `No such file or directory` for a non-existent adjacent `crates` directory. | Included `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/crates` in a scoped `rg` command. | Reused the successful `src/` hits and did not assume an adjacent `crates/` layout for this firmware repository. |
| Phase 6 eFuse/PMS/RNG discovery search failed with `No such file or directory` for `include/hw/xtensa/esp32s3.h`. | Included a guessed QEMU header path in a scoped `rg` command. | Used the existing `hw/xtensa/esp32s3.c`, `include/hw/nvram/*efuse.h`, `include/hw/misc/esp32s3_reg.h`, and ESP-IDF `reg_base.h` sources directly instead of assuming that header exists. |
| Initial Phase 6 eFuse qtest expected programmed block reads to update immediately after a program command. | The model writes successful programs to the persistent mirror and ESP-IDF performs a read command after programming to refresh read registers. | Updated the qtest helper to run an eFuse read command after successful programming before asserting guest-visible read-register contents. |
| Phase 6 RNG qtest crashed QEMU with signal 11 on a write to the read-only RNG region. | The RNG model had no write handler, so the qtest exposed an unsafe guest-write path instead of deterministic unsupported behavior. | Added an explicit no-op RNG write handler and kept qtest coverage proving writes do not crash and unsupported offsets remain zero. |
| Phase 7 plan referenced non-existent `tests/tcg/xtensa/Makefile.target`. | Tried to inspect the named file while confirming atomctl registration. | Corrected the plan to reference `tests/tcg/xtensa/Makefile.softmmu-target`, which wildcard-registers `test_s32c1i_atomctl.S`; no makefile edit was required. |
