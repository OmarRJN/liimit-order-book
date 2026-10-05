# Testing

A matching engine is a good candidate for rigorous testing. Its rules are precise, its state is small enough to reason about exhaustively, and its failures are subtle: a bug rarely crashes, it simply produces a trade that should not have happened. This document separates what is tested **today** from what is **planned**, so that the two are never confused.

## 1. Current state

`main()` in `orderbook.cpp` runs a short scenario with nine checks. It exits with a non-zero status on the first failure.

| Check | Behaviour covered |
|---|---|
| Two resting orders | Non-crossing orders rest and are counted |
| Best bid 100, best ask 105 | `BestBid` and `BestAsk` |
| Partial fill of 4 | A crossing order matches against the best level |
| 6 left at the best bid | Remaining quantity is reported correctly in depth |
| Market order fills for 5 | A market order sweeps available liquidity |
| Market remainder does not rest | The unfilled part of a market order is cancelled |
| Fill-or-kill rejected | A `FillOrKill` with no liquidity leaves the book untouched |
| Order repriced to 99 | `ModifyOrder` replaces an order |
| Book empty after cancel | `CancelOrder` removes the order and its level |

This is a smoke test. It demonstrates that the main paths work, but it does not establish correctness: it has no negative cases for most operations, it does not check the invariants in `DESIGN.md` §5.4, and a single scenario cannot explore interactions between operations.

## 2. Test strategy

Four layers, in order of cost and value.

1. **Example-based unit tests** for each rule, written so that a failure names the rule that broke.
2. **Invariant checks** run after every operation in every test.
3. **Randomised differential testing** against a deliberately naive reference model.
4. **Sanitisers** (address, undefined behaviour, thread) over all of the above.

Performance measurement is deliberately separate; see §5.

### 2.1 Framework

[Catch2](https://github.com/catchorg/Catch2) (v3) or GoogleTest. Either is acceptable. Catch2 is lighter to integrate and its `SECTION` blocks suit this domain, where many tests share the same setup of a book with a few resting orders. For that to work, the `Orderbook` class needs to move into `orderbook.hpp`, with the demonstration left in `main.cpp`. That refactor is the first item in the roadmap's testing milestone.

### 2.2 Invariant helper

Test code needs read access to the internal state to verify the invariants. Rather than make members public, add a `friend` test hook or a `CheckInvariants()` method compiled only when `ORDERBOOK_TESTING` is defined. It should assert:

1. best bid < best ask whenever both sides are non-empty;
2. no price level is empty;
3. `orders_.size()` equals the total number of orders across all levels, and every stored iterator dereferences to its own order;
4. `filled + remaining == initial` for each order, and no resting order has zero remaining quantity;
5. no identifier appears twice.

Every test below should call this helper after each operation, so that invariants are checked everywhere rather than in one dedicated test.

## 3. Unit test catalogue

Each row states the setup, the action and the expected outcome. Quantities and prices are illustrative.

### 3.1 Matching

| ID | Scenario | Expected |
|---|---|---|
| M1 | Buy 100×10, then sell 105×5 | No trades; two resting orders |
| M2 | Buy 100×10, then sell 100×10 | One trade of 10; book empty |
| M3 | Buy 100×10, then sell 100×4 | One trade of 4; buy remains with 6 and keeps its place |
| M4 | Two buys at 100 (ids 1, 2), then sell 100×5 | Trade is against id 1 (time priority) |
| M5 | Buys at 101 and 100, then sell 100×15 | The 101 level trades first (price priority) |
| M6 | Several ask levels, then a large crossing buy | Trades are produced best price first; levels removed as exhausted |
| M7 | Add an order with an identifier already live | No trades; book unchanged |
| M8 | Order that crosses by price but with remaining quantity after sweeping | Remainder rests at its own limit price |

### 3.2 Fill-and-kill

| ID | Scenario | Expected |
|---|---|---|
| F1 | Limit price does not reach the best opposing price (or the opposite side is empty) | Rejected; nothing rests |
| F2 | Opposing liquidity smaller than the order | Partial fill; remainder cancelled; book contains none of it |
| F3 | Opposing liquidity larger than the order | Fully filled |

### 3.3 Fill-or-kill

| ID | Scenario | Expected |
|---|---|---|
| K1 | Opposing liquidity smaller than requested | Rejected; book unchanged |
| K2 | Liquidity sufficient only when summed across several levels | Fills completely |
| K3 | Sufficient liquidity exists, but only at prices worse than the limit | Rejected |
| K4 | Available quantity exactly equals the request | Fills completely (boundary) |
| K5 | Empty opposite side | Rejected |

### 3.4 Market

| ID | Scenario | Expected |
|---|---|---|
| R1 | Opposite side empty | Rejected |
| R2 | Quantity smaller than total liquidity | Fills in full, best price first |
| R3 | Quantity larger than total liquidity | Fills what exists; remainder cancelled; nothing rests |
| R4 | Market order on each side | Behaviour is symmetric |

### 3.5 Cancel

| ID | Scenario | Expected |
|---|---|---|
| C1 | Cancel an unknown identifier | No effect, no throw |
| C2 | Cancel the middle order of a three-deep level | The other two keep their relative order |
| C3 | Cancel the only order at a level | The level is removed |
| C4 | Cancel an order already fully filled | No effect |

### 3.6 Modify

| ID | Scenario | Expected |
|---|---|---|
| D1 | Modify an unknown identifier | No trades; book unchanged |
| D2 | Modify to a price that crosses | Trades are produced |
| D3 | Modify a front-of-queue order without changing price | Order moves to the back (priority lost) |
| D4 | Modify a `FillAndKill`-typed order | Type is preserved |

### 3.7 Edge cases

| ID | Scenario | Expected |
|---|---|---|
| E1 | Zero-quantity order | Decision required: reject at the boundary. *Currently unhandled; to be specified and tested.* |
| E2 | Maximum price and quantity values | No overflow in matching or depth sums |
| E3 | Negative prices | Handled consistently, since `Price` is signed |

E1 and E3 expose underspecified behaviour rather than confirmed bugs. Writing the test forces a decision, which is its main value here.

## 4. Randomised differential testing

Example-based tests encode the cases one thought of. A randomised test finds the ones one did not.

**Reference model.** A naive implementation that stores all resting orders in a single `std::vector`, and on each operation finds matches by sorting by price, then arrival sequence. It is slow and obviously correct, which is the point.

**Procedure.**

1. Generate a seeded random stream of operations: add (random side, type, price within a narrow band, small quantity), cancel (an existing or non-existing identifier), modify.
2. Apply each operation to both the real book and the model.
3. After every operation, compare the returned trades (identifiers, prices, quantities, order) and the full depth snapshot.
4. Run the invariant helper on the real book.

A narrow price band and small quantities are important: they force frequent crossing, multi-level sweeps and partial fills, which is where the faults concentrate. On failure, print the seed and the operation index so that the case can be replayed and shrunk by hand.

## 5. Sanitisers and performance

**Sanitisers.** Build the test binary three ways:

```
-fsanitize=address,undefined      # memory errors, iterator misuse, signed overflow
-fsanitize=thread                 # data races (needs a multi-threaded test, below)
```

The map-node reference bug described in `DESIGN.md` §8 is exactly the kind of fault AddressSanitizer exists to catch.

**Concurrency test.** Several threads add and cancel concurrently against one book; afterwards, run the invariant helper and check that the number of live orders equals adds minus cancels minus fully filled orders. This tests that the lock discipline holds, not that results are deterministic.

**Benchmarks.** Kept apart from correctness tests so a slow machine never produces a spurious failure. Measure add, cancel and match throughput over a seeded synthetic workload, and report medians over repeated runs rather than a single figure. Compile with optimisation, pin the benchmark to a core where possible, and record the compiler version and hardware beside the results. A number without its conditions cannot be interpreted.

## 6. Build integration

A minimal `CMakeLists.txt` target, once the header split has been made:

```cmake
cmake_minimum_required(VERSION 3.20)
project(orderbook CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(Catch2 3 REQUIRED)

add_executable(orderbook_tests tests/orderbook_tests.cpp)
target_compile_definitions(orderbook_tests PRIVATE ORDERBOOK_TESTING)
target_compile_options(orderbook_tests PRIVATE -Wall -Wextra -fsanitize=address,undefined)
target_link_options(orderbook_tests PRIVATE -fsanitize=address,undefined)
target_link_libraries(orderbook_tests PRIVATE Catch2::Catch2WithMain)

enable_testing()
add_test(NAME orderbook_tests COMMAND orderbook_tests)
```

Continuous integration (GitHub Actions) should build with both GCC and Clang and run this target on every push.

## 7. Definition of done for the core

The core is considered tested when:

- every row in §3 has a passing test;
- the invariant helper runs after every operation in every test;
- the differential test has run for at least several million random operations without divergence across multiple seeds;
- the suite passes under address, undefined-behaviour and thread sanitisers.
