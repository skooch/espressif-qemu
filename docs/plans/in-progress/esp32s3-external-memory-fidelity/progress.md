# ESP32-S3 External Memory Fidelity Progress

- 2026-04-19: Promoted plan `#4` from `docs/plans/new/` to `docs/plans/in-progress/` on `codex/esp32s3-external-memory-fidelity`.
- 2026-04-19: Set the active execution target for this phase: replace immediate backing-store visibility with a functional coherency contract for flash and PSRAM aliases while keeping cache timing and replacement policy out of scope.
- 2026-04-19: Added the first coherency state scaffolding in `esp32s3_cache`: backing-page generation and visible-generation tracking for flash and PSRAM, reset/finalize lifecycle handling, and explicit stale-versus-visible helpers.
- 2026-04-19: Changed SPI1 flash mutation visibility so `esp32s3_cache_flash_modified()` marks mapped flash backing pages stale instead of immediately refreshing the EXTMEM alias mirror.
- 2026-04-19: Wired `ICACHE_SYNC` completion to refresh stale mapped flash pages and replaced the old direct qtest with a stale-before-sync and fresh-after-sync regression.
- 2026-04-19: Updated `test_xtensa_esp32s3_multicore_park_flash.py` so the embedded ROM now runs `ICACHE_SYNC` after SPI1 page-program and sector-erase operations before checking the EXTMEM alias, matching the promoted coherency boundary instead of the older immediate-refresh assumption.
