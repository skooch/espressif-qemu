# QEMU WiFi Follow-Up

**Status:** still unimplemented, but the original transport assumption is stale.

This plan now lives in the QEMU repo because the simulator-side device model
and host-network bridge are owned here. The original write-up assumed UART2
was available for the WiFi transport; that is no longer true now that the live
tree uses UART2 for the GPS emulator.

Treat the existing design docs here as a starting point, not as an exact
implementation recipe. The first step is to refresh the transport choice
against the current QEMU and firmware trees, then continue with the actual
emulation work.
