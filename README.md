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
- **Thread safety.** All public operations are guarded by a single mutex.

## Building

Requires a compiler with C++20 support (GCC 11+, Clang 14+ or MSVC 19.30+).

```bash
g++ -std=c++20 -Wall -Wextra -O2 orderbook.cpp -o orderbook
./orderbook
```

The executable runs a short demonstration and exits with a non-zero status if any check fails.

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

Prices are integers, measured in ticks. A rejected order and an order that was accepted but did not trade both return an empty `Trades`; distinguishing them is on the [roadmap](ROADMAP.md).

## Repository layout

Current:

```
orderbook.cpp    engine and demonstration
README.md        this file
DESIGN.md        motivation, prior art, architecture, trade-offs
TESTING.md       current coverage and test strategy
ROADMAP.md       planned work, in order
```

Planned (see Milestone 2 of the roadmap):

```
include/orderbook.hpp
src/main.cpp
tests/orderbook_tests.cpp
CMakeLists.txt
```

## Status

The core engine is complete and passes its smoke test. A full unit and randomised test suite is the next milestone; until it exists, the engine should be regarded as working but not yet verified. [TESTING.md](TESTING.md) is explicit about the difference.

## Acknowledgements

The overall structure follows the order book built on the YouTube channel [@TheCodingJesus](https://www.youtube.com/@TheCodingJesus) ([source repository](https://github.com/Tzadiko/Orderbook)). The additions and changes made here are listed in [DESIGN.md](DESIGN.md) §3.3.
