# 5. ESP32-S3 EMAC Fidelity Plan

## Goal
Decide how far to carry ESP32-S3 Ethernet fidelity beyond the current `open_eth` compatibility contract and land the next source-backed step without breaking the working board-path launch contract.

## Current Phase
Completed on 2026-04-19. This plan is now a historical record under `docs/plans/implemented/`.

Ideal backlog order: 5 of 7.

## Scope
This plan covers:

- the current `open_eth` wrapper contract and which parts of it are acceptable long-term for the active workload
- deeper EMAC descriptor, interrupt, MDIO/MII, and PHY-facing behavior if local sources justify tighter modeling
- whether a true ESP32-S3-specific EMAC block should replace or continue to wrap `open_eth`

This plan does not assume a full PHY or MAC rewrite is justified unless the local evidence or active firmware path requires it.

## Chosen Direction

Keep the ESP32-S3 board on the existing QEMU `open_eth` wrapper contract for now.

- The local ESP-IDF `components/esp_eth/src/openeth/esp_eth_mac_openeth.c` driver is already a QEMU-only OpenCores path rather than a real ESP32-S3 EMAC driver.
- The active adjacent T-Deck Pro workload is Wi-Fi-based and does not exercise EMAC or PHY bring-up directly.
- The next justified fidelity slice is therefore a tighter, source-backed OpenCores wrapper contract, not a dedicated ESP32-S3 EMAC replacement.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/net/opencores_eth.c`
- `hw/xtensa/esp32s3.c`
- `tests/qtest/esp32s3-test.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/esp_eth/src/openeth/esp_eth_mac_openeth.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm`

## File Map
- Modify: `hw/net/opencores_eth.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/emac`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`

## Tasks
- [x] Reconcile the current `open_eth` board-path contract with ESP32-S3-specific EMAC source material from the local corpus.
- [x] Decide that the next slice should remain a tighter wrapper contract rather than the start of a dedicated ESP32-S3 EMAC device.
- [x] Land the next source-backed MII/PHY behavior that current firmware and the local OpenETH driver can prove: `MIITX_DATA` now stages PHY control data, and the write only commits when `MIICOMMAND.WCTRLDATA` is asserted.
- [x] Keep the launch contract explicit so the board path does not regress while deeper EMAC work is still partial.
- [x] Update `ESP32S3_EMULATION_GAPS.md` after the chosen direction is clear.

## Exit Criteria
- The EMAC backlog has an explicit chosen direction instead of an ambiguous “replace open_eth later” note.
- Any promoted EMAC behavior is source-backed and qtest-covered.
