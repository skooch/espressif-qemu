# ESP32-S3 RMT Model Progress

- 2026-04-19: Activated plan `#6` from `docs/plans/new/` into `docs/plans/in-progress/`.
- 2026-04-19: Confirmed the adjacent firmware does not currently call into RMT. The local evidence is limited to ESP32-S3 SoC headers, generated register constants, interrupt-map entries, and TRM material, so the first justified slice stays a narrow modeled register surface rather than waveform timing or board-path behavior.
- 2026-04-19: Chosen first slice: replace the unimplemented RMT window with an explicit 0xd0 register model backed by the local ESP-IDF layout, APB-visible channel memory words, source-backed reset defaults and write masks, and immediate TX_END interrupt completion for TX start on channels 0-3.
- 2026-04-19: Landed the explicit `esp32s3.rmt` device, wired it into the ESP32-S3 SoC reset and interrupt map, and added direct qtests for the register surface plus immediate TX_END interrupt completion on TX start.
- 2026-04-19: Verified `git diff --check`, `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`, the focused `/xtensa/esp32s3/rmt` qtests, and the full `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test` floor before moving the plan to `docs/plans/implemented/`.
