# 4. ESP32-S3 External Memory Fidelity Plan

## Goal
Add a functional EXTMEM cache-coherency model so guest-visible flash and PSRAM alias behavior no longer depends on immediate backing-store refresh, while keeping cycle timing and replacement policy explicitly out of scope.

## Current Phase
Queued on 2026-04-19 under `docs/plans/new/`.

Ideal backlog order: 4 of 7.

## File Map
- Modify: `hw/misc/esp32s3_cache.c`
- Modify: `include/hw/misc/esp32s3_cache.h`
- Modify: `hw/ssi/esp32s3_spi.c`
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `tests/functional/test_xtensa_esp32s3_cache_reject.py`
- Modify: `tests/functional/test_xtensa_esp32s3_multicore_park_flash.py`
- Modify: `ESP32S3_EMULATION_GAPS.md`

## Phases

### Phase 1: Coherency Contract And State Model
- [ ] Re-audit the local TRM, ESP-IDF cache/MMU sources, and current guest paths to define the functional coherency contract this model will claim: when flash/PSRAM alias data may stay stale, which maintenance operations make backing-store mutations visible, and which behaviors remain intentionally unmodeled.
- [ ] Extend `include/hw/misc/esp32s3_cache.h` with explicit per-domain cache-line metadata and any source-backed constants needed for line tracking, separating line state from MMU page translation state.
- [ ] Add the line-state helpers in `hw/misc/esp32s3_cache.c` that can look up, mark stale, invalidate, refill, and flush line metadata without yet changing maintenance-operation wiring.
- [ ] Record the narrowed contract in `ESP32S3_EMULATION_GAPS.md`, explicitly stating that functional coherency is in scope while replacement policy, contention, stalls, and flash-controller micro-timing remain out of scope.
- **Status:** pending

### Phase 2: Backing Store Decoupling
- [ ] Change the flash mutation path in `hw/misc/esp32s3_cache.c` so `esp32s3_cache_flash_modified()` no longer makes mutated flash contents immediately visible through every mapped alias; instead it must mark affected lines or pages stale according to the new line-state model.
- [ ] Update `hw/ssi/esp32s3_spi.c` only as needed so page-program and erase callbacks preserve the new “backing store changed, cache visibility pending maintenance” contract without regressing existing SPI1 behavior.
- [ ] Add the equivalent state transition for PSRAM-backed aliases in `hw/misc/esp32s3_cache.c`, so writes through one path can leave cached alias lines stale until the modeled maintenance path resolves them.
- [ ] Preserve the existing MMU permission, reject, and fault behavior while decoupling cache-visible data from the backing `flash_as` / `psram_as` mappings.
- **Status:** pending

### Phase 3: Maintenance Operations With Real Coherency Effects
- [ ] Wire the existing EXTMEM maintenance controls in `hw/misc/esp32s3_cache.c` so `SYNC`, `PRELOAD`, `AUTOLOAD`, `FREEZE`, suspend/resume, and MMU invalidation paths update line visibility and stale/valid state instead of only toggling busy/done bits.
- [ ] Keep the current deferred completion and freeze-drain ordering contract intact while making completion correspond to real coherency state transitions.
- [ ] Re-audit `hw/xtensa/esp32s3.c` for any SoC-owned reset or suspend path that must drop or reset cache-line state to preserve the current reset-domain contract.
- [ ] Update `ESP32S3_EMULATION_GAPS.md` to state the exact coherency behaviors now claimed and the exact residual items still blocked for accuracy.
- **Status:** pending

### Phase 4: Regression Coverage For Stale And Fresh Visibility
- [ ] Extend `tests/qtest/esp32s3-test.c` with direct cache qtests that prove stale-then-fresh behavior for flash aliases, MMU remaps, and maintenance operations rather than only register completion bits.
- [ ] Extend `tests/qtest/esp32s3-test.c` with direct cache qtests that prove PSRAM-backed alias visibility changes only after the modeled coherency path.
- [ ] Keep `tests/functional/test_xtensa_esp32s3_cache_reject.py` focused on reject/fault behavior, widening it only if the functional coherency work changes the board-level reject contract.
- [ ] Update `tests/functional/test_xtensa_esp32s3_multicore_park_flash.py` so the board-level flash probe proves the intended coherency boundary instead of only immediate mapped-alias refresh.
- **Status:** pending

### Phase 5: Documentation And Exit
- [ ] Re-read `ESP32S3_EMULATION_GAPS.md` and ensure the external-memory section now distinguishes resolved functional coherency from still-blocked microarchitectural fidelity.
- [ ] Append the implementation log to the plan progress record when this plan moves to `docs/plans/in-progress/` and later to `docs/plans/implemented/`.
- [ ] Keep the exit scoped to functional coherency. Do not expand the plan to cycle timing, replacement policy, contention, or flash-controller micro-timing without a separate follow-on decision.
- **Status:** pending

## Decisions
| Decision | Rationale |
|----------|-----------|
| Scope this plan to functional coherency, not cycle-accurate cache hardware. | This resolves the concrete developer complaint without pretending to have vendor-grade microarchitectural references. |
| Keep MMU permission, reject, and fault logic as the existing contract baseline. | The gap is data visibility and cache state, not the current access-control plumbing. |
| Treat flash and PSRAM coherency as one plan. | The missing abstraction is shared: alias-visible data must be decoupled from backing-store mutation in both paths. |

## Errors
| Error | Attempt | Resolution |
|-------|---------|------------|

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/cache`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_multicore_park_flash.py`

## Exit Criteria
- Flash and PSRAM alias visibility no longer updates solely because the backing store changed; it follows the documented functional coherency contract.
- Direct qtests prove stale-before-maintenance and fresh-after-maintenance behavior for the promoted cache paths.
- The remaining external-memory gap is reduced to explicitly documented microarchitectural and timing fidelity items rather than “coherency not modeled.”
