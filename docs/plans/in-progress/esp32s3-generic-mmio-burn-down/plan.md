# 3. ESP32-S3 Generic MMIO Burn-Down Plan

## Goal
Keep shrinking the generic MMIO fallback so new guest-visible behavior is owned by explicit device models instead of hidden behind compatibility storage.

## Current Phase
Activated on 2026-04-19 under `docs/plans/in-progress/`.

Ideal backlog order: 3 of 7.

## Scope
This plan covers:

- remaining stored-readback fallback windows such as LEDC and blocked radio/internal ranges
- explicit ownership decisions for currently deferred but source-backed windows
- trace-driven replacement of new in-scope catch-all hits with narrow modeled devices or RAZ/WI behavior
- keeping unimplemented RMT and other deferred owners visible rather than silently compatible

This phase does not claim analog fidelity for radios or unsupported peripheral internals once they move out of fallback storage.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/xtensa/esp32s3.c`
- `tests/qtest/esp32s3-test.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc`
- active trace output produced through `esp32s3_unimplemented_io_read` and `esp32s3_unimplemented_io_write`

## File Map
- Modify: `hw/xtensa/esp32s3.c`
- Modify: any newly owned peripheral source selected by the trace-driven follow-on
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/generic-mmio`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`

## Tasks
- [ ] Re-run the generic-MMIO trace classification when a new guest path or firmware revision touches the fallback.
- [ ] Split source-backed deferred windows, such as LEDC or any new in-scope owner, out of the generic fallback into narrow explicit models.
- [ ] Keep blocked radio/internal windows explicit in the docs instead of letting them drift into accidental compatibility behavior.
- [ ] Ensure new catch-all addresses in core boot, reset, sleep, cache, clock, interrupt, eFuse, PMS, RNG, or Xtensa paths are treated as fidelity bugs.
- [ ] Update `ESP32S3_EMULATION_GAPS.md` after each burn-down slice so the fallback boundary stays reviewable.

## Exit Criteria
- The generic MMIO fallback is smaller, more explicit, and less likely to mask new fidelity bugs.
- Every remaining compatibility-storage range is intentionally named in the docs and tied to a deferred owner phase.
