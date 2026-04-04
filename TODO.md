# Deferred Foundation Priorities

- [x] Task 1: finish the clock-update path so RTC clock changes affect dependent board behavior
- [x] Task 1: add direct qtests for wake, reset, and stall transitions
- [x] Task 1: inventory the remaining clock/reset shortcuts still masking real state transitions
- [x] Task 2: inventory the cache/MMU operations the firmware depends on today
- [x] Task 2: add regressions for the guest-visible ordering guarantees
- [x] Task 2: separate immediate state changes from deferred completion semantics
- [x] Task 2: leave broader cycle-accuracy work for a later stage
- [ ] Task 4: record which missing Xtensa instructions or local-memory behaviors actually block guest code
- [ ] Task 4: rank those blockers so the smallest guest-visible fixes land first
- [ ] Task 3: inventory the exact EMAC behavior the firmware touches
- [ ] Task 3: decide between a replacement model and a board adapter around the existing path
- [ ] Task 4: implement one architectural slice at a time with a focused regression for each
- [ ] Task 4: keep this track separate from peripheral work so the dependency chain stays visible
- [ ] Task 3: implement the minimum packet and link bring-up behavior needed by firmware
- [ ] Task 3: add a regression for link bring-up and packet behavior
