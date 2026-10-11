# Limit Order Book

A single-instrument limit order book and matching engine in C++20, with price-time priority, partial fills and four order types.

The project is deliberately narrow. Rather than a broad but shallow trading system, it aims to get the details that matter in practice right: matching priority, partial-fill bookkeeping, the semantics of each order type, and the invariants that must hold between operations. The reasoning is set out in [DESIGN.md](DESIGN.md).

## Features

- **Price-time priority.** Better prices trade first; within a price, the earliest order trades first.
- **Order types.**
  - `GoodTillCancel`: rests until filled or cancelled.
  - `FillAndKill`: matches what it can immediately and cancels the remainder.
  - `FillOrKill`: executes in full immediately, or not at all.
  - `Market`: takes liquidity at any price; any remainder is cancelled.
- **Operations.** Add, cancel and modify. A modification is a cancel-and-replace and therefore forfeits queue position.
- **Market views.** Best bid, best ask, and aggregated quantity per price level.
- **Input validation.** Orders with a quantity of zero are rejected, as are duplicate identifiers. A modification to zero quantity is refused and leaves the original order untouched.
- **Thread safety.** All public operations are guarded by a single mutex.

## Building

Requires a compiler with C++20 support (GCC 11+, Clang 14+ or MSVC 19.30+).

```bash
g++ -std=c++20 -Wall -Wextra -O2 orderbook.cpp -o orderbook
./orderbook
```

The executable runs a short demonstration (eleven checks) and exits with a non-zero status if any of them fails.

To run the full test harness, which includes the randomised comparison against a reference model:

```bash
g++ -std=c++20 -Wall -Wextra -O2 tests.cpp -o tests -pthread
./tests
```

`tests.cpp` includes `orderbook.cpp` directly, so the two files must sit side by side. Do not add both to the same Visual Studio project, because each defines `main`.

Visual Studio users can open `OrderBook.vcxproj`, which builds `orderbook.cpp` as a C++20 console application. I have not been able to build the project file with MSVC myself; the engine itself was built and tested with GCC 13 and Clang 18.

## Usage

```cpp
Orderbook book;

// A resting buy: 10 lots at 100.
book.AddOrder(std::make_shared<Order>(
    OrderType::GoodTillCancel, /*id*/ 1, Side::Buy, /*price*/ 100, /*quantity*/ 10));

// An aggressive sell that crosses it: trades 4 lots.
Trades trades = book.AddOrder(std::make_shared<Order>(
    OrderType::GoodTillCancel, 2, Side::Sell, 100, 4));

for (const auto& trade : trades)
    std::cout << trade.GetBidTrade().quantity << " lots traded\n";

// A market buy of 5 lots (no price: the book assigns one).
book.AddOrder(std::make_shared<Order>(/*id*/ 3, Side::Buy, /*quantity*/ 5));

// Reprice order 1, then cancel it.
book.ModifyOrder(OrderModify{1, Side::Buy, 99, 6});
book.CancelOrder(1);

// Inspect the book.
if (auto bid = book.BestBid()) std::cout << "best bid: " << *bid << '\n';
auto depth = book.GetOrderInfos();   // depth.GetBids(), depth.GetAsks()
```

Prices are integers, measured in ticks. A rejected order and an order that was accepted but did not trade both return an empty `Trades`; distinguishing them is on the [roadmap](ROADMAP.md). Level totals returned by `GetOrderInfos()` are 64-bit, so they cannot wrap even with very large orders.

## Repository layout

Current:

```
orderbook.cpp              engine and demonstration
tests.cpp                  unit tests, invariant checks, differential and concurrency tests
OrderBook.vcxproj          Visual Studio project (builds orderbook.cpp)
OrderBook.vcxproj.filters  Visual Studio solution-explorer layout
README.md                  this file
DESIGN.md                  motivation, prior art, architecture, trade-offs
TESTING.md                 current coverage and test strategy
ROADMAP.md                 planned work, in order
```

Planned (see Milestone 2 of the roadmap):

```
include/orderbook.hpp
src/main.cpp
tests/orderbook_tests.cpp
CMakeLists.txt
```

## Status

The core engine is complete. It passes the demonstration, a catalogue of unit tests, and two million randomised operations checked against a naive reference model, with the structural invariants verified after every operation. It is clean under the address, undefined-behaviour and thread sanitisers, and builds without warnings on GCC and Clang.

What it does not yet have is the packaging that makes this repeatable for everyone else: the tests are a plain-assertion harness rather than a Catch2 suite, there is no CMake build, and nothing runs in continuous integration. Those are the remaining items in Milestone 2 of the [roadmap](ROADMAP.md). [TESTING.md](TESTING.md) sets out precisely what has and has not been checked.

## Acknowledgements

The overall structure follows the order book built on the YouTube channel [@TheCodingJesus](https://www.youtube.com/@TheCodingJesus) ([source repository](https://github.com/Tzadiko/Orderbook)). The additions and changes made here are listed in [DESIGN.md](DESIGN.md) §3.3.
