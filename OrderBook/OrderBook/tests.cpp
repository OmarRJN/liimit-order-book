// tests.cpp
// Test harness for orderbook.cpp: unit tests, invariant checks, a randomised
// differential test against a naive reference model, and a concurrency test.
//
// Build:  g++ -std=c++20 -Wall -Wextra -O2 tests.cpp -o tests -pthread
// Sanitisers:
//   g++ -std=c++20 -g -O1 -fsanitize=address,undefined tests.cpp -o tests_asan -pthread
//   g++ -std=c++20 -g -O1 -fsanitize=thread            tests.cpp -o tests_tsan -pthread
//
// A note on the includes below. The engine keeps its state private, and I
// want to check the internal invariants (DESIGN.md section 5.4) after every
// single operation, so this file opens the class up with a macro. That is a
// hack and it is only acceptable here, in a test, never in the engine. The
// tidier route is the ORDERBOOK_TESTING hook described in TESTING.md, which
// becomes worth doing once the engine moves into its own header.

#define main orderbook_demo_main      // keep the engine's demo main() out of the way
#define private public                // see the note above
#include "orderbook.cpp"
#undef private
#undef main

#include <atomic>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>

static int failures = 0;

#define EXPECT(cond, msg) \
    do { if (!(cond)) { ++failures; std::cout << "[FAIL] " << msg << "  (line " << __LINE__ << ")\n"; } } while (0)

// ---------------------------------------------------------------------------
// Invariants (DESIGN.md section 5.4). Returns an empty string if all hold,
// otherwise a short description of the first one that does not.
// ---------------------------------------------------------------------------
std::string CheckInvariants(const Orderbook& b)
{
    // 1. The book is never crossed.
    if (!b.bids_.empty() && !b.asks_.empty() && b.bids_.begin()->first >= b.asks_.begin()->first)
        return "book is crossed";

    std::size_t total = 0;
    std::set<OrderId> ids;

    auto scan = [&](const auto& side, Side s) -> std::string
        {
            for (const auto& [price, level] : side)
            {
                if (level.empty()) return "empty price level";                                  // 2
                for (auto it = level.begin(); it != level.end(); ++it)
                {
                    const auto& o = *it;
                    ++total;
                    if (o->GetRemainingQuantity() == 0) return "resting order with zero remaining";   // 4
                    if (o->GetFilledQuantity() + o->GetRemainingQuantity() != o->GetInitialQuantity())
                        return "quantity not conserved";                                                // 4
                    if (o->GetPrice() != price || o->GetSide() != s) return "order sitting in the wrong level";
                    if (o->GetOrderType() == OrderType::FillAndKill) return "FillAndKill order left resting";
                    if (!ids.insert(o->GetOrderId()).second) return "duplicate order id";             // 5
                    auto f = b.orders_.find(o->GetOrderId());
                    if (f == b.orders_.end()) return "order missing from the index";                  // 3
                    if (f->second.location != it) return "index iterator points at the wrong node";   // 3
                }
            }
            return "";
        };

    if (auto e = scan(b.bids_, Side::Buy); !e.empty()) return e;
    if (auto e = scan(b.asks_, Side::Sell); !e.empty()) return e;
    if (total != b.orders_.size()) return "index size differs from the number of resting orders";      // 3
    return "";
}

#define INV(book) EXPECT(CheckInvariants(book).empty(), "invariant broken: " + CheckInvariants(book))

// ---------------------------------------------------------------------------
// Naive reference model.
// Everything lives in one vector and every question is answered by scanning it.
// It is slow and deliberately dull, which is the point: it is easy to see that
// it is right, so any disagreement with the real book points at the real book.
// ---------------------------------------------------------------------------
struct ModelOrder { OrderId id; Side side; Price price; Quantity remaining; OrderType type; std::uint64_t seq; };

struct ModelTrade
{
    OrderId bid, ask; Price bidPrice, askPrice; Quantity quantity;
    bool operator==(const ModelTrade&) const = default;
};

struct Model
{
    std::vector<ModelOrder> orders;
    std::uint64_t nextSeq = 0;

    bool has(OrderId id) const { for (auto& o : orders) if (o.id == id) return true; return false; }

    static bool opposite(const ModelOrder& o, Side s, Price p)
    {
        return s == Side::Buy ? (o.side == Side::Sell && o.price <= p) : (o.side == Side::Buy && o.price >= p);
    }

    bool canMatch(Side s, Price p) const { for (auto& o : orders) if (opposite(o, s, p)) return true; return false; }

    std::vector<ModelTrade> add(OrderType type, OrderId id, Side s, Price p, Quantity q, bool market = false)
    {
        if (q == 0 || has(id)) return {};
        if (market)
        {
            bool found = false; Price worst = 0;
            for (auto& o : orders)
                if (o.side != s) { if (!found || (s == Side::Buy ? o.price > worst : o.price < worst)) worst = o.price; found = true; }
            if (!found) return {};
            p = worst; type = OrderType::FillAndKill;
        }
        if (type == OrderType::FillAndKill && !canMatch(s, p)) return {};
        if (type == OrderType::FillOrKill)
        {
            std::uint64_t available = 0;
            for (auto& o : orders) if (opposite(o, s, p)) available += o.remaining;
            if (!canMatch(s, p) || available < q) return {};
        }

        orders.push_back({ id, s, p, q, type, nextSeq++ });

        std::vector<ModelTrade> trades;
        for (;;)
        {
            // Best bid: highest price, then earliest. Best ask: lowest price, then earliest.
            ModelOrder* bb = nullptr; ModelOrder* ba = nullptr;
            for (auto& o : orders)
            {
                if (o.side == Side::Buy)
                { if (!bb || o.price > bb->price || (o.price == bb->price && o.seq < bb->seq)) bb = &o; }
                else
                { if (!ba || o.price < ba->price || (o.price == ba->price && o.seq < ba->seq)) ba = &o; }
            }
            if (!bb || !ba || bb->price < ba->price) break;

            const Quantity m = std::min(bb->remaining, ba->remaining);
            bb->remaining -= m; ba->remaining -= m;
            trades.push_back({ bb->id, ba->id, bb->price, ba->price, m });

            const OrderId a = bb->id, c = ba->id;
            std::erase_if(orders, [&](const ModelOrder& o) { return (o.id == a || o.id == c) && o.remaining == 0; });
        }

        // A FillAndKill order never rests, wherever it ended up.
        std::erase_if(orders, [](const ModelOrder& o) { return o.type == OrderType::FillAndKill; });
        return trades;
    }

    void cancel(OrderId id) { std::erase_if(orders, [&](const ModelOrder& o) { return o.id == id; }); }

    std::vector<ModelTrade> modify(OrderId id, Side s, Price p, Quantity q)
    {
        if (q == 0) return {};   // refused, and the original order stays put
        OrderType t{}; bool found = false;
        for (auto& o : orders) if (o.id == id) { t = o.type; found = true; }
        if (!found) return {};
        cancel(id);
        return add(t, id, s, p, q);
    }

    std::map<Price, TotalQuantity> depth(Side s) const
    {
        std::map<Price, TotalQuantity> d;
        for (auto& o : orders) if (o.side == s) d[o.price] += o.remaining;
        return d;
    }
};

std::vector<ModelTrade> convert(const Trades& t)
{
    std::vector<ModelTrade> r;
    for (auto& x : t)
        r.push_back({ x.GetBidTrade().orderId, x.GetAskTrade().orderId,
                      x.GetBidTrade().price, x.GetAskTrade().price, x.GetBidTrade().quantity });
    return r;
}

bool depthMatches(const Orderbook& b, const Model& m)
{
    auto info = b.GetOrderInfos();
    std::map<Price, TotalQuantity> bids, asks;
    for (auto& l : info.GetBids()) bids[l.price] = l.quantity;
    for (auto& l : info.GetAsks()) asks[l.price] = l.quantity;
    return bids == m.depth(Side::Buy) && asks == m.depth(Side::Sell);
}

// ---------------------------------------------------------------------------
// Randomised differential test.
// A narrow price band, small quantities and a small id range are chosen on
// purpose: they force constant crossing, multi-level sweeps, partial fills,
// duplicate ids and cancels of unknown orders, which is where bugs live.
// Zero quantities are included so the rejection paths get exercised too.
// On a failure the seed and operation number are printed so it can be replayed.
// ---------------------------------------------------------------------------
bool Differential(std::uint64_t seed, int operations, bool verbose)
{
    std::mt19937_64 rng(seed);
    auto R = [&](int lo, int hi) { return static_cast<int>(lo + rng() % static_cast<std::uint64_t>(hi - lo + 1)); };

    Orderbook book;
    Model model;

    for (int i = 0; i < operations; ++i)
    {
        const int kind = R(0, 99);
        std::vector<ModelTrade> got, want;
        std::ostringstream desc;

        if (kind < 62)
        {
            const OrderId id = static_cast<OrderId>(R(1, 150));
            const Side s = R(0, 1) ? Side::Buy : Side::Sell;
            const Price p = R(95, 105);
            const Quantity q = static_cast<Quantity>(R(0, 12));
            const int ty = R(0, 9);

            if (ty < 5)      { desc << "add GTC "; got = convert(book.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, id, s, p, q))); want = model.add(OrderType::GoodTillCancel, id, s, p, q); }
            else if (ty < 7) { desc << "add FAK "; got = convert(book.AddOrder(std::make_shared<Order>(OrderType::FillAndKill, id, s, p, q)));   want = model.add(OrderType::FillAndKill, id, s, p, q); }
            else if (ty < 9) { desc << "add FOK "; got = convert(book.AddOrder(std::make_shared<Order>(OrderType::FillOrKill, id, s, p, q)));    want = model.add(OrderType::FillOrKill, id, s, p, q); }
            else             { desc << "add MKT "; got = convert(book.AddOrder(std::make_shared<Order>(id, s, q)));                                 want = model.add(OrderType::Market, id, s, 0, q, true); }
            desc << "id=" << id << " price=" << p << " qty=" << q;
        }
        else if (kind < 82)
        {
            const OrderId id = static_cast<OrderId>(R(1, 150));
            desc << "cancel id=" << id;
            book.CancelOrder(id);
            model.cancel(id);
        }
        else
        {
            const OrderId id = static_cast<OrderId>(R(1, 150));
            const Side s = R(0, 1) ? Side::Buy : Side::Sell;
            const Price p = R(95, 105);
            const Quantity q = static_cast<Quantity>(R(0, 12));
            desc << "modify id=" << id << " price=" << p << " qty=" << q;
            got = convert(book.ModifyOrder(OrderModify{ id, s, p, q }));
            want = model.modify(id, s, p, q);
        }

        const std::string inv = CheckInvariants(book);
        const bool tradesAgree = (got == want);
        const bool depthAgrees = depthMatches(book, model);

        if (!inv.empty() || !tradesAgree || !depthAgrees || book.Size() != model.orders.size())
        {
            std::cout << "[DIVERGENCE] seed=" << seed << " operation #" << i << ": " << desc.str()
                      << "\n   invariant: " << (inv.empty() ? "ok" : inv)
                      << "\n   trades agree: " << tradesAgree << "   depth agrees: " << depthAgrees
                      << "   size engine/model: " << book.Size() << "/" << model.orders.size() << "\n";
            return false;
        }
    }

    if (verbose) std::cout << "  seed " << seed << ": " << operations << " operations, no divergence\n";
    return true;
}

// ---------------------------------------------------------------------------
int main()
{
    constexpr Quantity kMaxQty = std::numeric_limits<Quantity>::max();
    constexpr Price    kMaxPrice = std::numeric_limits<Price>::max();

    auto G = [](OrderId id, Side s, Price p, Quantity q) { return std::make_shared<Order>(OrderType::GoodTillCancel, id, s, p, q); };
    auto T = [](OrderType t, OrderId id, Side s, Price p, Quantity q) { return std::make_shared<Order>(t, id, s, p, q); };

    std::cout << "Unit tests (IDs refer to TESTING.md section 3):\n";

    // ---- 3.1 Matching ----
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 10)); auto t = b.AddOrder(G(2, Side::Sell, 105, 5));
      EXPECT(t.empty() && b.Size() == 2, "M1 non-crossing orders rest"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 10)); auto t = b.AddOrder(G(2, Side::Sell, 100, 10));
      EXPECT(t.size() == 1 && t[0].GetBidTrade().quantity == 10 && b.Size() == 0, "M2 exact fill empties the book"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 10)); b.AddOrder(G(2, Side::Sell, 100, 4));
      EXPECT(b.Size() == 1 && b.GetOrderInfos().GetBids()[0].quantity == 6, "M3 partial fill leaves 6"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 5)); b.AddOrder(G(2, Side::Buy, 100, 5)); auto t = b.AddOrder(G(3, Side::Sell, 100, 5));
      EXPECT(t.size() == 1 && t[0].GetBidTrade().orderId == 1, "M4 time priority"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 10)); b.AddOrder(G(2, Side::Buy, 101, 10)); auto t = b.AddOrder(G(3, Side::Sell, 100, 15));
      EXPECT(t.size() == 2 && t[0].GetBidTrade().orderId == 2 && t[1].GetBidTrade().orderId == 1, "M5 price priority"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 101, 2)); b.AddOrder(G(2, Side::Sell, 102, 3)); b.AddOrder(G(3, Side::Sell, 103, 4));
      auto t = b.AddOrder(G(4, Side::Buy, 103, 8));
      EXPECT(t.size() == 3 && t[0].GetAskTrade().price == 101 && t[1].GetAskTrade().price == 102 && t[2].GetAskTrade().price == 103
             && t[2].GetAskTrade().quantity == 3 && b.BestAsk() == 103 && b.GetOrderInfos().GetAsks()[0].quantity == 1,
             "M6 multi-level sweep, best price first, exhausted levels removed"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 5)); auto t = b.AddOrder(G(1, Side::Sell, 90, 5));
      EXPECT(t.empty() && b.Size() == 1 && b.BestAsk() == std::nullopt, "M7 duplicate id ignored"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 3)); auto t = b.AddOrder(G(2, Side::Buy, 101, 10));
      EXPECT(t.size() == 1 && b.BestBid() == 101 && b.GetOrderInfos().GetBids()[0].quantity == 7, "M8 remainder rests at its own limit"); INV(b); }

    // ---- 3.2 Fill-and-kill ----
    { Orderbook b; auto t = b.AddOrder(T(OrderType::FillAndKill, 1, Side::Buy, 100, 5));
      EXPECT(t.empty() && b.Size() == 0, "F1 FAK on an empty book rejected"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 105, 5)); auto t = b.AddOrder(T(OrderType::FillAndKill, 2, Side::Buy, 100, 5));
      EXPECT(t.empty() && b.Size() == 1, "F1 FAK that cannot reach the best ask rejected"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 3)); auto t = b.AddOrder(T(OrderType::FillAndKill, 2, Side::Buy, 100, 10));
      EXPECT(t.size() == 1 && t[0].GetBidTrade().quantity == 3 && b.Size() == 0, "F2 FAK partial fill, remainder cancelled"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 10)); auto t = b.AddOrder(T(OrderType::FillAndKill, 2, Side::Buy, 100, 4));
      EXPECT(t.size() == 1 && b.Size() == 1 && b.GetOrderInfos().GetAsks()[0].quantity == 6 && b.BestBid() == std::nullopt, "F3 FAK filled in full"); INV(b); }

    // ---- 3.3 Fill-or-kill ----
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 3)); auto t = b.AddOrder(T(OrderType::FillOrKill, 2, Side::Buy, 100, 5));
      EXPECT(t.empty() && b.Size() == 1 && b.GetOrderInfos().GetAsks()[0].quantity == 3, "K1 insufficient liquidity: rejected, book untouched"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 3)); b.AddOrder(G(2, Side::Sell, 101, 3)); auto t = b.AddOrder(T(OrderType::FillOrKill, 3, Side::Buy, 101, 6));
      EXPECT(t.size() == 2 && b.Size() == 0, "K2 liquidity summed across levels"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 102, 10)); auto t = b.AddOrder(T(OrderType::FillOrKill, 2, Side::Buy, 101, 5));
      EXPECT(t.empty() && b.Size() == 1, "K3 liquidity only at worse prices: rejected"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 5)); auto t = b.AddOrder(T(OrderType::FillOrKill, 2, Side::Buy, 100, 5));
      EXPECT(t.size() == 1 && b.Size() == 0, "K4 exact quantity is enough (boundary)"); INV(b); }
    { Orderbook b; auto t = b.AddOrder(T(OrderType::FillOrKill, 1, Side::Sell, 100, 5));
      EXPECT(t.empty() && b.Size() == 0, "K5 FOK on an empty side rejected"); INV(b); }

    // ---- 3.4 Market ----
    { Orderbook b; auto t = b.AddOrder(std::make_shared<Order>(1, Side::Buy, 5));
      EXPECT(t.empty() && b.Size() == 0, "R1 market order with an empty opposite side rejected"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 3)); b.AddOrder(G(2, Side::Sell, 101, 3)); auto t = b.AddOrder(std::make_shared<Order>(3, Side::Buy, 4));
      EXPECT(t.size() == 2 && t[0].GetAskTrade().quantity == 3 && t[0].GetAskTrade().price == 100
             && t[1].GetAskTrade().quantity == 1 && t[1].GetAskTrade().price == 101 && b.Size() == 1, "R2 market buy fills best price first"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 4)); auto t = b.AddOrder(std::make_shared<Order>(2, Side::Buy, 20));
      EXPECT(t.size() == 1 && t[0].GetBidTrade().quantity == 4 && b.Size() == 0, "R3 market buy: remainder cancelled, nothing rests"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 4)); b.AddOrder(G(2, Side::Buy, 99, 4)); auto t = b.AddOrder(std::make_shared<Order>(3, Side::Sell, 20));
      EXPECT(t.size() == 2 && b.Size() == 0, "R3/R4 market sell sweeps, remainder cancelled (symmetry)"); INV(b); }

    // ---- 3.5 Cancel ----
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 1)); b.CancelOrder(42);
      EXPECT(b.Size() == 1, "C1 cancelling an unknown id changes nothing"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 1)); b.AddOrder(G(2, Side::Buy, 100, 1)); b.AddOrder(G(3, Side::Buy, 100, 1)); b.CancelOrder(2);
      auto t = b.AddOrder(G(4, Side::Sell, 100, 2));
      EXPECT(t.size() == 2 && t[0].GetBidTrade().orderId == 1 && t[1].GetBidTrade().orderId == 3, "C2 cancelling the middle order keeps the others in order"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 1)); b.AddOrder(G(2, Side::Buy, 99, 1)); b.CancelOrder(1);
      EXPECT(b.BestBid() == 99 && b.GetOrderInfos().GetBids().size() == 1, "C3 cancelling the only order removes the level"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 5)); b.AddOrder(G(2, Side::Sell, 100, 5)); b.CancelOrder(1);
      EXPECT(b.Size() == 0, "C4 cancelling an already filled order is a no-op"); INV(b); }

    // ---- 3.6 Modify ----
    { Orderbook b; auto t = b.ModifyOrder(OrderModify{ 9, Side::Buy, 100, 1 });
      EXPECT(t.empty() && b.Size() == 0, "D1 modify of an unknown id changes nothing"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, 5)); b.AddOrder(G(2, Side::Buy, 99, 5)); auto t = b.ModifyOrder(OrderModify{ 2, Side::Buy, 100, 5 });
      EXPECT(t.size() == 1 && b.Size() == 0, "D2 modify to a crossing price trades"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, 1)); b.AddOrder(G(2, Side::Buy, 100, 1)); b.ModifyOrder(OrderModify{ 1, Side::Buy, 100, 1 });
      auto t = b.AddOrder(G(3, Side::Sell, 100, 1));
      EXPECT(t.size() == 1 && t[0].GetBidTrade().orderId == 2, "D3 modified order loses time priority"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 105, 5)); b.AddOrder(T(OrderType::FillAndKill, 2, Side::Buy, 100, 5));   // rejected: cannot match
      auto t = b.ModifyOrder(OrderModify{ 2, Side::Buy, 105, 5 });
      EXPECT(t.empty() && b.Size() == 1, "D4 a FillAndKill never rests, so its id cannot be modified"); INV(b); }

    // ---- 3.7 Edge cases ----
    { Orderbook b; auto t = b.AddOrder(G(1, Side::Buy, 100, 0));
      EXPECT(t.empty() && b.Size() == 0, "E1 zero-quantity limit order rejected"); INV(b);
      t = b.AddOrder(std::make_shared<Order>(2, Side::Buy, 0));
      EXPECT(t.empty() && b.Size() == 0, "E1 zero-quantity market order rejected"); INV(b);
      b.AddOrder(G(3, Side::Sell, 100, 3)); b.AddOrder(G(4, Side::Buy, 99, 3));
      t = b.AddOrder(G(5, Side::Buy, 100, 0));
      EXPECT(t.empty() && b.Size() == 2, "E1 zero-quantity order does not trade against a real one"); INV(b);
      t = b.ModifyOrder(OrderModify{ 4, Side::Buy, 99, 0 });
      EXPECT(t.empty() && b.Size() == 2 && b.BestBid() == 99 && b.GetOrderInfos().GetBids()[0].quantity == 3,
             "E1 zero-quantity modify refused, original order untouched"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, kMaxPrice, kMaxQty)); auto t = b.AddOrder(G(2, Side::Sell, kMaxPrice, kMaxQty));
      EXPECT(t.size() == 1 && t[0].GetBidTrade().quantity == kMaxQty && b.Size() == 0, "E2 maximum price and quantity match without overflow"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, 100, kMaxQty)); b.AddOrder(G(2, Side::Buy, 100, kMaxQty));
      EXPECT(b.GetOrderInfos().GetBids()[0].quantity == 2ull * kMaxQty, "E2 level total does not wrap past 32 bits"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Sell, 100, kMaxQty)); b.AddOrder(G(2, Side::Sell, 100, kMaxQty));
      auto t = b.AddOrder(T(OrderType::FillOrKill, 3, Side::Buy, 100, kMaxQty));
      EXPECT(t.size() == 1 && b.Size() == 1, "E2 fill-or-kill liquidity check does not overflow"); INV(b); }
    { Orderbook b; b.AddOrder(G(1, Side::Buy, -5, 3)); auto t = b.AddOrder(G(2, Side::Sell, -5, 3));
      EXPECT(t.size() == 1 && b.Size() == 0, "E3 negative prices trade consistently"); INV(b); }

    // ---- README usage example, verbatim, so the documentation cannot drift ----
    {
        Orderbook book;
        book.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10));
        Trades trades = book.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, 2, Side::Sell, 100, 4));
        EXPECT(trades.size() == 1 && trades[0].GetBidTrade().quantity == 4, "README example: 4 lots traded");
        book.AddOrder(std::make_shared<Order>(3, Side::Buy, 5));
        book.ModifyOrder(OrderModify{ 1, Side::Buy, 99, 6 });
        book.CancelOrder(1);
        auto depth = book.GetOrderInfos();
        EXPECT(depth.GetBids().empty() && depth.GetAsks().empty() && !book.BestBid(), "README example: book ends empty");
        INV(book);
    }

    // ---- Differential testing ----
    std::cout << "Differential tests (engine vs naive model, invariants after every operation):\n";
    int diverged = 0;
    for (std::uint64_t seed = 1; seed <= 40; ++seed)
        if (!Differential(seed, 50000, seed <= 3)) ++diverged;
    std::cout << "  40 seeds x 50,000 operations = 2,000,000 operations; seeds that diverged: " << diverged << "\n";
    EXPECT(diverged == 0, "differential test");

    // ---- Concurrency ----
    // Four threads hammer one book with adds, cancels and snapshots. We cannot
    // predict the outcome, only that the lock discipline keeps the book
    // consistent. Run it under -fsanitize=thread to catch data races.
    {
        Orderbook book;
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t)
            workers.emplace_back([&book, t]
                {
                    std::mt19937 rng(static_cast<unsigned>(t + 1));
                    for (int i = 0; i < 20000; ++i)
                    {
                        const OrderId id = static_cast<OrderId>(t) * 1000000 + static_cast<OrderId>(i);
                        book.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, id,
                            (rng() & 1) ? Side::Buy : Side::Sell, static_cast<Price>(95 + rng() % 11), static_cast<Quantity>(1 + rng() % 5)));
                        if (rng() % 3 == 0) book.CancelOrder(static_cast<OrderId>(t) * 1000000 + rng() % static_cast<unsigned>(i + 1));
                        if (rng() % 7 == 0) (void)book.GetOrderInfos();
                    }
                });
        for (auto& w : workers) w.join();
        const std::string inv = CheckInvariants(book);
        EXPECT(inv.empty(), "concurrent run left the book inconsistent: " + inv);
        std::cout << "Concurrency: 4 threads x 20,000 adds finished, invariants " << (inv.empty() ? "hold" : inv) << "\n";
    }

    std::cout << (failures ? "\nSOME TESTS FAILED\n" : "\nALL TESTS PASSED\n");
    return failures ? 1 : 0;
}
