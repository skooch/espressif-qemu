# ESP32-S3 Generic MMIO Burn-Down Progress

## 2026-04-19

- Activated plan `#3` and moved it to `docs/plans/in-progress/`.
- Initial target is the remaining source-backed LEDC range, because an explicit LEDC device model already exists in-tree and the generic fallback still owns that window on ESP32-S3.
- Remaining generic compatibility storage will be reclassified after the LEDC owner handoff so the docs and qtests only claim the truly deferred FE/FE2/NRX/BB windows.
