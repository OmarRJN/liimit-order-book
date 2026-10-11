# Roadmap

The order of work follows one principle: **establish correctness before pursuing speed.** Optimising an untested matching engine produces a fast system whose output cannot be trusted, and any performance claim made about it is hollow. Milestones are therefore ordered so that each one makes the next safe to attempt.

Items are intentions, not commitments, and are likely to change as measurements arrive.

## Milestone 1: Core engine (complete)

- [x] Price-time priority matching with partial fills
- [x] `GoodTillCancel`, `FillAndKill`, `FillOrKill` and `Market` order types
- [x] Add, cancel and modify
- [x] Best bid, best ask and depth snapshot
- [x] Mutex-guarded public interface
- [x] Smoke-test scenario in `main()`
- [x] Design document (`DESIGN.md`)
- [x] Validation of zero-quantity orders and modifications

## Milestone 2: Verification

Goal: be able to state, with evidence, that the engine is correct.

Done, in the plain-assertion harness `tests.cpp`:

- [x] Unit tests covering every case in `TESTING.md` §3
- [x] Invariant checker, run after every operation in tests (currently by opening up the class with a macro; the proper `CheckInvariants()` hook is still to do)
- [x] Specify and test zero-quantity and boundary inputs (E1–E3)
- [x] Randomised differential test against a naive reference model
- [x] Address, undefined-behaviour and thread sanitiser runs (by hand)

Still to do:

- [ ] Split `Orderbook` into `orderbook.hpp` and a thin `main.cpp`
- [ ] CMake build with warnings enabled
- [ ] Port the unit tests to Catch2, using `SECTION` blocks for the shared setups
- [ ] A proper `CheckInvariants()` hook compiled under `ORDERBOOK_TESTING`, replacing the macro
- [ ] Extend the differential test beyond two million operations, and the concurrency test to check live-order counts
- [ ] Continuous integration on GCC and Clang, running the sanitiser builds

This milestone is the most valuable on the list. The project's stated aim is correctness in the details, and this is where that aim is demonstrated rather than asserted. The checking itself is now largely done; what remains is making it repeatable by anyone, and automatic.

## Milestone 3: Semantics

Goal: close the behavioural gaps recorded in `DESIGN.md` §7.

- [ ] Record a single execution price per trade (the resting order's price) in place of each side's limit price
- [x] Widen level-quantity accumulation to 64 bits
- [ ] `GoodForDay` orders, with a session-end cut-off. This is mostly a design question: it needs a clock abstraction so that expiry can be tested without waiting
- [ ] Reject invalid orders explicitly, returning a status instead of an empty trade list. At present, "rejected" and "accepted but did not trade" are indistinguishable to the caller

## Milestone 4: Measurement

Goal: replace intuition about performance with numbers.

- [ ] Synthetic workload generator (seeded; configurable mix of add, cancel and marketable orders)
- [ ] Benchmark harness reporting median and tail latency, with hardware and compiler recorded
- [ ] Profile with `perf` or equivalent to find where time actually goes
- [ ] Per-level running totals, to remove the linear sums in the fill-or-kill check and depth snapshot

Per-level totals sit here, not in Milestone 3, because the change is justified only if the profile shows those sums matter, and it is safe only once Milestone 2's tests exist.

## Milestone 5: Performance engineering

Each change is to be accepted only if it improves a measured result and leaves the full test suite passing.

- [ ] Pool allocator for orders, replacing per-order `shared_ptr` allocation
- [ ] Single-threaded engine fed by a sequenced queue, as an alternative to the mutex
- [ ] Compare the map-of-lists layout against a bounded array indexed by tick
- [ ] Intrusive order lists to remove the separate index iterator

The aim of this milestone is as much to learn what the standard library already does well as to beat it. A negative result, such as "the array layout was no faster on this workload", is a legitimate outcome and worth recording.

## Milestone 6: Stretch

Only worth attempting once the above are done.

- [ ] Stop and stop-limit orders
- [ ] Self-trade prevention
- [ ] Replay of recorded market data (for example, a public ITCH sample) through the book
- [ ] A simple simulated market maker, to exercise the engine under realistic order flow
- [ ] Multiple instruments behind a router

## Deliberately not planned

Networking, persistence and a front end are excluded. They are substantial and would dilute the focus on matching correctness.
