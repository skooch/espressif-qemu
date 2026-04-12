# Plan: ESP32-S3 OpenEth Networking

## Goal
Tighten the `esp32s3` machine's existing `open_eth` networking path so the QEMU-side contract is explicit, diagnosable, and regression-tested for a firmware-managed `in_qemu()` EMAC backend.

## Current Phase
Complete

## File Map
- Modify: `hw/xtensa/esp32s3.c`
- Modify: `tests/qtest/esp32s3-test.c`
- Modify: `ESP32S3_EMULATION_GAPS.md`
- Modify: `docs/plans/new/qemu-wifi/design.md`
- Modify: `docs/plans/new/qemu-wifi/plan.md`

## Phases

### Phase 1: Refresh The QEMU Networking Contract
- [x] Update `ESP32S3_EMULATION_GAPS.md` to state that the active QEMU-side networking surface is Ethernet-only via `open_eth`, not radio or UART-backed Wi-Fi emulation.
- [x] Add the exact launch contract to `ESP32S3_EMULATION_GAPS.md`: `-nic user,id=emac0,model=open_eth` is the supported host-backed backend for the `esp32s3` machine.
- [x] Update `docs/plans/new/qemu-wifi/design.md` to mark the UART2/chardev Wi-Fi design as superseded for the current direction, citing the existing `open_eth` path and the UART2 GPS conflict.
- [x] Update `docs/plans/new/qemu-wifi/plan.md` to mark its transport and QEMU-side implementation steps as stale for this slice so later agents do not revive them by accident.
- [x] Record the scope boundary in the docs: firmware-side transport selection and external `tdeck qemu run` wiring are handled outside this repo.
- **Status:** complete

### Phase 2: Add Board-Level Diagnostics Without Changing The Architecture
- [x] In `hw/xtensa/esp32s3.c`, change `esp32s3_init_openeth()` so a missing NIC config does not fail silently; emit a single startup warning that names the required launch shape and leaves the machine bootable.
- [x] Keep the board architecture unchanged: do not auto-create a backend, do not add a new Wi-Fi device, and do not repurpose UART2.
- [x] Make the warning text precise enough for debugging: explain that no `open_eth` device was instantiated because no matching NIC configuration was supplied for the board.
- [x] Preserve the current MMIO and IRQ wiring exactly when `open_eth` is configured so the existing guest-visible contract does not drift.
- **Status:** complete

### Phase 3: Lock The Contract Down With QTests And Targeted Verification
- [x] Extend `tests/qtest/esp32s3-test.c` with an `info qtree`-based assertion that `-M esp32s3 -nic none` does not instantiate `open_eth`.
- [x] Extend `tests/qtest/esp32s3-test.c` with an `info qtree`-based assertion that `-M esp32s3 -nic user,id=emac0,model=open_eth` does instantiate `open_eth`.
- [x] Keep the existing `/esp32s3/emac/link-loopback` coverage as the behavioral contract for MII link state, interrupt signaling, and descriptor loopback once the device is present.
- [x] Run the targeted ESP32-S3 qtest binary to cover the new instantiation assertions plus the existing EMAC link-loopback case.
- [x] Run one manual board launch for each path: `-nic none` to verify the warning, and `-nic user,id=emac0,model=open_eth` to verify the warning disappears.
- **Status:** complete

## Decisions
| Decision | Rationale |
|----------|-----------|
| Keep the QEMU slice Ethernet-only through `open_eth` | The board already has a tested EMAC path, while UART2 Wi-Fi transport work conflicts with the live GPS emulator and duplicates firmware-side abstraction work. |
| Warn instead of hard-failing when no NIC backend is supplied | Networkless board runs should remain possible, but the current silent skip is too hard to diagnose for the planned firmware EMAC backend. |
| Refresh the stale `qemu-wifi` planning docs as part of this slice | Leaving the older UART2/chardev plan untouched invites future implementation drift back toward the wrong architecture. |
| Leave firmware work and external simulator wrapper changes out of scope | The user is handling the firmware adapter and CLI/MCP wiring separately; this repo should focus on the board contract it owns. |

## Errors
| Error | Attempt | Resolution |
|-------|---------|------------|
| None | - | - |
