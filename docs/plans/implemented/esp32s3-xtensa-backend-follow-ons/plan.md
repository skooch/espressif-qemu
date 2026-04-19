# 7. ESP32-S3 Xtensa Backend Follow-Ons Plan

## Goal
Track and fix the remaining Xtensa backend gaps only when they are backed by exact local ISA/configured-core references or reproduced guest failures.

## Current Phase
Completed on 2026-04-19 and archived under `docs/plans/implemented/`.

Ideal backlog order: 7 of 7.

## Scope
This plan covers:

- the remaining reject-surface gaps and any reproduced opcode or TIE failures on the active ESP32-S3 guest path
- evidence gathering from the local Xtensa corpora before changing backend behavior
- keeping the current `ATOMCTL` and board-path regressions green while adding new backend coverage

This phase does not authorize speculative configured-core, TIE, or SIMD implementation without exact evidence.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `docs/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md`
- `target/xtensa`
- `tests/tcg/xtensa`
- `tests/functional/test_xtensa_esp32s3_cache_reject.py`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/xtensa`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm`

## File Map
- Modify: `target/xtensa`
- Modify: `tests/tcg/xtensa`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `tests/functional/test_xtensa_esp32s3_cache_reject.py`
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Modify: `docs/plans/implemented/esp32s3-deferred-foundation/xtensa-blockers.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/cache`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`
- local Xtensa TCG regression through the documented machine-specific fallback path when backend code changes
- `QEMU_TEST_QEMU_BINARY=build/qemu-system-xtensa QEMU_BUILD_ROOT=build PYTHONPATH=python:tests/functional uv run --with pycotap python3 tests/functional/test_xtensa_esp32s3_cache_reject.py`

## Tasks
- [x] Keep a ranked queue of reproduced ESP32-S3 Xtensa backend failures tied to exact guest evidence or local manual references.
- [x] Revisit the remaining reject-surface gaps only when a board-path failure or focused test proves they matter.
- [x] Use the local Xtensa corpus before widening configured-core, TIE, or SIMD support.
- [x] Add or widen TCG, qtest, or functional coverage for each newly fixed backend path.
- [x] Keep `ESP32S3_EMULATION_GAPS.md` and `xtensa-blockers.md` aligned after each promoted backend slice.

## Exit Criteria
- Xtensa backend work stays evidence-driven rather than speculative.
- Each promoted backend fix has a direct reproduced failure and a focused regression to keep it pinned.
- No additional backend slice is promoted until a new guest failure or exact configured-core reference appears.
