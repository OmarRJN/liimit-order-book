# Design: Limit Order Book

This document records what the order book does, how it is built, and why it is built that way. It is written to be read top to bottom, but each section stands on its own.

| Document | Purpose |
|---|---|
| [README.md](README.md) | What the project is, how to build and run it |
| **DESIGN.md** (this file) | Motivation, prior art, architecture, trade-offs |
| [TESTING.md](TESTING.md) | Current coverage and the planned test strategy |
| [ROADMAP.md](ROADMAP.md) | What is finished, what is next, and why in that order |

---

## 1. Motivation

A limit order book is the core data structure underpinning every modern exchange — it holds the live, unfilled buy and sell orders for an instrument and determines when and how they match. It sits at the intersection of two things I wanted to get properly comfortable with: systems-level C++ (manual memory reasoning, container selection under performance constraints) and the mechanics of financial markets technology, which is the area I'm aiming my internship applications toward.

Most beginner projects in this space stop at "orders go in, trades come out." The aim here is to build something narrower in scope but correct in the details that actually matter in practice — price-time priority, partial fills, and order types beyond a plain limit order — rather than something broad but shallow.

## 2. Scope

### In scope

- A single-instrument book with price-time priority matching.
- Four order types: `GoodTillCancel`, `FillAndKill`, `FillOrKill` and `Market`.
- Add, cancel and modify operations, with partial fills.
- Read-only views of the book: best bid, best ask and aggregated depth per price level.
- Coarse-grained thread safety, so the book can be shared between threads without data races.

### Out of scope (for now)

- Multiple instruments, or any form of symbol routing.
- Networking, market-data protocols (FIX, ITCH) and persistence.
- Stop orders, iceberg orders, self-trade prevention and auctions.
- Latency-grade performance engineering. Correctness comes first; the measurable trade-offs are discussed in section 6 and scheduled in the [roadmap](ROADMAP.md).

The exclusions are deliberate. Each of them is a project in its own right, and adding them superficially would work against the stated aim of getting the core details right.

## 3. Prior art

### 3.1 The reference implementation

The project's structure follows the order book written on the YouTube channel [@TheCodingJesus](https://www.youtube.com/@TheCodingJesus), whose code is published at [Tzadiko/Orderbook](https://github.com/Tzadiko/Orderbook). That series is the best-known C++ walkthrough of this problem at an approachable level, and it is the baseline against which this project should be judged. The choices inherited from it are:

- two ordered maps keyed by price, one per side, each mapping a price to a FIFO queue of orders;
- a hash map from order identifier to the order and its position in its queue, which makes cancellation cheap;
- the `OrderType` vocabulary and the split between `Order`, `OrderModify`, `Trade` and `Orderbook`.

I worked through the series to understand it, then rebuilt the code and extended it. Being explicit about this matters: the value of this repository lies in the understanding and the extensions, not in having invented the underlying design.

### 3.2 Other designs surveyed

Two alternative approaches illustrate what the chosen design gives up.

| Approach | Core idea | Strength | Cost |
|---|---|---|---|
| **Map of price → list** (this project; the reference above) | Standard-library ordered map, one list per level | Simple, hard to misuse, easy to reason about | Pointer-heavy; poor cache locality; `O(log P)` level access |
| **Balanced tree of price limits with intrusive order lists** (for example [brprojects/Limit-Order-Book](https://github.com/brprojects/Limit-Order-Book)) | Hand-built tree of price levels; orders link to each other and to their parent level directly | Fewer allocations; cached best bid and ask pointers; the author reports over a million operations per second | Considerably more code; manual lifetime management and rebalancing |
| **Array indexed by price** (a classic design from early matching-engine competitions) | Price is an array index; each slot is a list head; memory comes from a fixed arena | `O(1)` level lookup and excellent locality | Requires a bounded, tick-aligned price range; wastes memory on sparse books |

### 3.3 What this project changes

Relative to the reference, the additions are:

1. `FillOrKill` and `Market` order types, including the liquidity check that `FillOrKill` requires.
2. A mutex-guarded public interface in which composite operations (modify) cannot lock recursively.
3. `BestBid`, `BestAsk` and a snapshot of aggregated depth, returned by value.
4. A set of defects fixed during the first pass and a later review; these are listed in section 8 because they illustrate the failure modes of this kind of code.
5. A written invariant set and test plan, in [TESTING.md](TESTING.md), with a harness (`tests.cpp`) that implements them.
6. Validation at the boundary: zero-quantity orders and zero-quantity modifications are refused (section 5.5).

## 4. Architecture

### 4.1 Data structures

```
bids_  : std::map<Price, std::list<OrderPointer>, std::greater<Price>>
asks_  : std::map<Price, std::list<OrderPointer>, std::less<Price>>
orders_: std::unordered_map<OrderId, { OrderPointer, list::iterator }>

        asks_ (lowest first)         bids_ (highest first)
        ┌─────┬──────────────┐       ┌─────┬──────────────┐
  best→ │ 101 │ [A7][A9]     │       │ 100 │ [B1][B4][B6] │ ←best
        │ 102 │ [A3]         │       │  99 │ [B2]         │
        │ 105 │ [A5][A8]     │       │  97 │ [B3][B5]     │
        └─────┴──────────────┘       └─────┴──────────────┘
                 ▲                              ▲
                 └──────── orders_ ─────────────┘
              id → (order, iterator into its level's list)
```

Three properties of this layout carry most of the design:

- **`begin()` is always the best price.** The comparators (`greater` for bids, `less` for asks) sort each map so that the best level comes first. Matching never has to search.
- **Each level is a `std::list`.** List iterators are not invalidated by insertion or erasure elsewhere in the list, so the iterator stored in `orders_` stays valid for the lifetime of the order. That is what makes cancellation independent of queue length.
- **Arrival order is queue order.** New orders are appended to the back of their level, and matching consumes from the front. Time priority is therefore a structural property rather than something computed from timestamps.

### 4.2 Types

Prices are signed 32-bit integers measured in ticks, not floating-point numbers. Floating-point prices make equality comparison unreliable, and equality is exactly what a matching engine depends on. The quantity of a single order is unsigned 32-bit, with `Order::Fill` throwing if asked to fill more than remains, so that an unsigned underflow cannot pass unnoticed. Anything that *adds* quantities together (a level's total, or the liquidity a fill-or-kill order needs) uses a separate 64-bit `TotalQuantity`, so two maximal orders at the same price cannot wrap the sum. Order identifiers are unsigned 64-bit.

`Market` orders have no price. They carry a sentinel (`InvalidPrice`) until the book assigns one on entry.

### 4.3 Complexity

`P` is the number of distinct price levels, `N` the number of resting orders and `L` the number of levels examined.

| Operation | Cost | Notes |
|---|---|---|
| Add (no match) | `O(log P)` | Map lookup or insert; list append and hash insert are `O(1)` |
| Match | `O(1)` per fill, plus `O(log P)` per exhausted level | Erasing `begin()` by iterator avoids a second search |
| Cancel | `O(log P)` | Hash lookup and list erase are `O(1)`; the `at(price)` lookup to reach the level is logarithmic |
| Modify | cost of cancel + cost of add | Implemented as cancel-and-replace |
| Best bid / best ask | `O(1)` | `begin()` on the relevant map |
| Fill-or-kill pre-check | `O(L · n)` worst case | Sums each level on demand; see section 6 |
| Depth snapshot | `O(N)` | Sums every level on demand |

## 5. Behaviour

### 5.1 Matching

After every insertion the book is matched. While the best bid is at or above the best ask:

1. Take the front order at each of the two best levels.
2. Fill both for the smaller of their remaining quantities.
3. Remove any order that is now filled, and remove any level that is now empty.
4. Record a `Trade` containing both sides.

Because bids and asks are matched at the front of their queues, the older order at a price always trades first (time priority), and a better-priced level always trades before a worse one (price priority).

### 5.2 Order types

| Type | On entry | Remainder |
|---|---|---|
| `GoodTillCancel` | Matches if it crosses | Rests on the book |
| `FillAndKill` | Rejected outright if it cannot match at all; otherwise matches what it can | Cancelled |
| `FillOrKill` | Rejected unless the full quantity is available at its limit price or better | Not applicable: it fills completely or not at all |
| `Market` | Priced at the worst opposing level, so it can sweep the book; rejected if the opposite side is empty | Cancelled (it is converted to `FillAndKill`) |

A fill-and-kill order that partially matches will, by construction, be the front order of the best level on its side once matching stops. The cleanup step therefore only inspects the front of each best level, rather than scanning the book. The reasoning is that a crossing order can only exist because nothing at or beyond its own price remains on its own side, so it must have become the best level. That is the sort of argument that is easy to get subtly wrong, so it is also checked empirically: the randomised test in [TESTING.md](TESTING.md) §4 compares the engine against a model that removes every fill-and-kill remainder wherever it sits, and has shown no divergence.

### 5.3 Modification

A modification cancels the existing order and adds a replacement with the same identifier and order type. The replacement joins the back of its queue, so the order **loses time priority**. This mirrors the behaviour of many real venues, where changing price or increasing size forfeits queue position. The reason to prefer it is fairness: allowing a modification to keep its place would let participants enlarge a position without queuing again.

The cost of the choice is that a modification is not atomic with respect to the market. Because the whole operation takes the book's lock, no other thread observes the intermediate state.

A modification to a quantity of zero is refused before anything is cancelled, and the original order stays exactly as it was. The alternative, treating it as a cancel, would let a malformed request silently delete an order. A caller who wants to cancel should say so.

### 5.4 Invariants

These must hold between any two public operations. They are the basis of the checks described in [TESTING.md](TESTING.md).

1. **Uncrossed book.** If both sides are non-empty, best bid < best ask.
2. **No empty levels.** Every price level present in a map holds at least one order.
3. **Index consistency.** `orders_` contains exactly the orders that appear in the level queues, and each stored iterator points at its own order.
4. **Quantity conservation.** For every order, filled + remaining = initial, and no resting order has zero remaining quantity.
5. **Unique identifiers.** No two live orders share an `OrderId`.

### 5.5 Validation

An order is refused, with no effect on the book, if its pointer is null, if its quantity is zero, or if its identifier is already live. The check happens before the order is touched, so a rejected market order is never converted to a limit order. A zero-quantity order is refused because it has no sensible meaning: left to rest it would break invariant 4, and left to match it would report a trade of zero lots.

Rejection is currently silent: the caller receives an empty list of trades, which is also what an accepted order that did not trade looks like. Returning a status is planned (see the [roadmap](ROADMAP.md)).

## 6. Design decisions and trade-offs

**Ordered map rather than a flat array.** A map handles an unbounded, sparse price range with no configuration, at the cost of pointer chasing. For a learning project whose goal is correctness, that is the right default. If benchmarking showed level lookup to be the bottleneck, a bounded array indexed by tick would be the first replacement to try.

**`std::list` per level.** A list gives stable iterators, which is the property the cancellation index depends on. A `std::deque` or `std::vector` would be faster to traverse but would invalidate positions on erase, forcing either lazy deletion or a linear search.

**`std::shared_ptr<Order>`.** Orders are shared between the level queue and the index. Reference counting is the simplest way to express that, but it costs an allocation and atomic operations per order. Since ownership is in fact singular (the book owns every order), a pooled allocator with raw or intrusive pointers is the natural next step once measurement justifies it.

**One coarse mutex.** Matching is inherently sequential, because each fill depends on the state left by the previous one. Finer-grained locking would add complexity without enabling real parallelism. Many production engines go further and run the book on a single thread fed by a sequenced queue, which removes locking from the hot path entirely. A mutex is the honest, simple choice at this stage.

**On-demand level sums.** `FillOrKill` pre-checks and depth snapshots currently sum each level's orders when asked. Maintaining a running total per level makes both cheap, at the cost of keeping that total consistent through every add, cancel and fill. It is a good optimisation, but only after the tests described in [TESTING.md](TESTING.md) are packaged and running automatically, because an inconsistent running total is exactly the kind of fault a randomised comparison finds and a human reviewer does not.

## 7. Known limitations

- **Execution price reporting.** Each side of a `Trade` records its own limit price. For a market order converted to a limit at the worst opposing price, the reported price is therefore the sweep limit, not the price at which it executed. Recording a single execution price (conventionally that of the resting order) would be more useful to a consumer and is on the roadmap.
- **Memory.** Orders are heap-allocated individually; there is no pooling.
- **No time-based expiry.** There is no `GoodForDay` order type or session handling.
- **Single instrument.** One `Orderbook` object represents one market.

## 8. Defects found and fixed

These are recorded because they are representative of the mistakes this kind of code invites. The first table comes from the initial pass; the second from a later review that compiled and tested the finished code.

### 8.1 First pass

The initial version of the code did not compile, and beneath the syntax errors were several logic faults.

| Defect | Consequence | Fix |
|---|---|---|
| `Fill` tested `quantity = remaining` (assignment) instead of `quantity > remaining` | Every fill overwrote state and threw | Corrected comparison |
| `TradeInfo` initialised as `{id, price, quantity}` but declared as `{id, quantity, price}` | Prices and quantities silently swapped in trade records | Single field order, and brace-initialisation checked against it |
| A level was erased by key while a reference into that very map node was still in use | Undefined behaviour | Erase by iterator, and re-bind references on each loop pass |
| Fill-and-kill cleanup sat inside the wrong scope | Remainders could rest on the book | Moved after the matching loop |
| `AddOrder` returned from inside its own duplicate check, leaving the rest unreachable | Orders never inserted | Restructured control flow |
| Constructor and call-site argument orders disagreed (`OrderModify`, `Order`) | Wrong fields populated | Aligned signatures |

### 8.2 Second pass

A later review built the project from scratch and exercised it with the harness in `tests.cpp`. It found two faults in the engine and several in the packaging.

| Defect | Consequence | Fix |
|---|---|---|
| A second, unfinished `main()` was left at the end of the source file, using types that did not exist | The file did not compile with any compiler | Removed |
| Zero-quantity orders were accepted | They rested with nothing remaining (breaking invariant 4) and later produced trades of zero lots | Rejected at the boundary; zero-quantity modifications also refused before any cancel (5.5) |
| Level totals and the fill-or-kill liquidity sum were 32-bit | Two large orders at one price could wrap the total, giving wrong depth and a wrong fill-or-kill decision | A 64-bit `TotalQuantity` for every sum |
| The Visual Studio project listed four source files that were not in the repository | The project could not build | Project now lists the single real source file |
| The documentation called the source `orderbook.cpp` while the project shipped it as `main.cpp` | Build instructions did not match the files | One name, `orderbook.cpp`, throughout |

The zero-quantity case had been flagged in the testing plan as an undecided behaviour (E1). Writing the test is what forced the decision, which is the argument for writing such tests early.
## 9. Further reading

- Prior art repositories linked in section 3.
- The commentary in `orderbook.cpp`, which explains individual choices at the point they are made.
