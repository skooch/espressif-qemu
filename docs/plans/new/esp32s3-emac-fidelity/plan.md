# ESP32-S3 EMAC Fidelity Plan

## Goal
Decide how far to carry ESP32-S3 Ethernet fidelity beyond the current `open_eth` compatibility contract and land the next source-backed step without breaking the working board-path launch contract.

## Current Phase
Queued on 2026-04-19 under `docs/plans/new/`.

## Scope
This plan covers:

- the current `open_eth` wrapper contract and which parts of it are acceptable long-term for the active workload
- deeper EMAC descriptor, interrupt, MDIO/MII, and PHY-facing behavior if local sources justify tighter modeling
- whether a true ESP32-S3-specific EMAC block should replace or continue to wrap `open_eth`

This plan does not assume a full PHY or MAC rewrite is justified unless the local evidence or active firmware path requires it.

## Evidence Base
- `ESP32S3_EMULATION_GAPS.md`
- `hw/net/open_eth.c`
- `hw/xtensa/esp32s3.c`
- `tests/qtest/esp32s3-test.c`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/external-resources/esp-idf/components/soc/esp32s3/register/soc`
- `/Users/skooch/projects/tdeck-pro-rust/tdeck-pro-rust/docs/trm`

## File Map
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `hw/net/open_eth.c` or a new ESP32-S3-specific EMAC device if promoted
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`

## Verification Floor
- `git diff --check`
- `ninja -C build qemu-system-xtensa tests/qtest/esp32s3-test`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test -p /xtensa/esp32s3/emac`
- `QTEST_QEMU_BINARY=./build/qemu-system-xtensa ./build/tests/qtest/esp32s3-test`

## Tasks
- [ ] Reconcile the current `open_eth` board-path contract with ESP32-S3-specific EMAC source material from the local corpus.
- [ ] Decide whether the next slice should be a tighter wrapper contract or the start of a dedicated EMAC device.
- [ ] Land only the next source-backed descriptor, interrupt, or MII/PHY behavior that current firmware or tests can prove.
- [ ] Keep the launch contract explicit so the board path does not regress while deeper EMAC work is still partial.
- [ ] Update `ESP32S3_EMULATION_GAPS.md` after the chosen direction is clear.

## Exit Criteria
- The EMAC backlog has an explicit chosen direction instead of an ambiguous “replace open_eth later” note.
- Any promoted EMAC behavior is source-backed and qtest-covered.
