// orderbook.cpp
// A single-instrument limit order book with price-time priority matching.
//
// How matching works, in one paragraph: the best bid is matched against the
// best ask for as long as the two cross (bid >= ask). Within a single price
// level orders are filled in arrival order, so the oldest order always trades
// first. Better prices always trade before worse ones.
//
// Supported order types:
//   GoodTillCancel : rests on the book until it is filled or cancelled.
//   FillAndKill    : matches whatever it can straight away; the rest is cancelled.
//   FillOrKill     : executes in full straight away, or not at all.
//   Market         : takes liquidity at any price; any unfilled remainder is cancelled.
//
// Orders with a quantity of zero are rejected, as is anything else that cannot
// be accepted. Rejection is silent for now: the caller gets back an empty list
// of trades, which is also what "accepted but did not trade" looks like. The
// roadmap has an item to fix that properly with a status code.
//
// Build:  g++ -std=c++20 -Wall -Wextra -O2 orderbook.cpp -o orderbook
// Run:    ./orderbook   (prints a short demonstration, exits non-zero on failure)

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------

enum class OrderType
{
    GoodTillCancel,
    FillAndKill,
    FillOrKill,
    Market
};

enum class Side
{
    Buy,
    Sell
};

// Prices are whole numbers of ticks. I deliberately avoided floating point:
// a matching engine lives or dies on "is this price equal to that one", and
// doubles make that question unreliable.
using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

// Sums of quantities (a whole price level, or the liquidity a fill-or-kill
// order needs) can exceed what one order can hold, so they get 64 bits.
// With 32 bits two large orders at the same price could silently wrap round.
using TotalQuantity = std::uint64_t;

// A market order has no price of its own, so until the book gives it one it
// carries this sentinel instead.
constexpr Price InvalidPrice = std::numeric_limits<Price>::min();

// ---------------------------------------------------------------------------
// Aggregated view of the book (price level -> total resting quantity)
// ---------------------------------------------------------------------------

struct LevelInfo
{
    Price         price;
    TotalQuantity quantity;
};

using LevelInfos = std::vector<LevelInfo>;

class OrderbookLevelInfos
{
public:
    OrderbookLevelInfos(LevelInfos bids, LevelInfos asks)
        : bids_{ std::move(bids) }, asks_{ std::move(asks) } {
    }

    const LevelInfos& GetBids() const { return bids_; }
    const LevelInfos& GetAsks() const { return asks_; }

private:
    LevelInfos bids_;
    LevelInfos asks_;
};

// ---------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------

class Order
{
public:
    // Limit-style order: GoodTillCancel, FillAndKill or FillOrKill.
    Order(OrderType type, OrderId id, Side side, Price price, Quantity quantity)
        : type_{ type }, id_{ id }, side_{ side }, price_{ price },
        initialQuantity_{ quantity }, remainingQuantity_{ quantity } {
    }

    // Market order: no price yet. The book works one out when the order arrives.
    Order(OrderId id, Side side, Quantity quantity)
        : Order(OrderType::Market, id, side, InvalidPrice, quantity) {
    }

    OrderId   GetOrderId() const { return id_; }
    Side      GetSide() const { return side_; }
    Price     GetPrice() const { return price_; }
    OrderType GetOrderType() const { return type_; }
    Quantity  GetInitialQuantity() const { return initialQuantity_; }
    Quantity  GetRemainingQuantity() const { return remainingQuantity_; }
    Quantity  GetFilledQuantity() const { return initialQuantity_ - remainingQuantity_; }
    bool      IsFilled() const { return remainingQuantity_ == 0; }

    void Fill(Quantity quantity)
    {
        // Quantity is unsigned, so over-filling would not go negative, it would
        // wrap to a huge number and carry on quietly. Better to fail loudly.
        if (quantity > remainingQuantity_)
            throw std::logic_error("Order cannot be filled for more than its remaining quantity");
        remainingQuantity_ -= quantity;
    }

    // Gives a market order a real limit price (the worst price on the opposite
    // side) and turns it into FillAndKill. The effect is that it can sweep the
    // whole book but can never end up resting on it.
    void ConvertMarketToLimit(Price price)
    {
        if (type_ != OrderType::Market)
            throw std::logic_error("Only market orders can be converted to limit orders");
        price_ = price;
        type_ = OrderType::FillAndKill;
    }

private:
    OrderType type_;
    OrderId   id_;
    Side      side_;
    Price     price_;
    Quantity  initialQuantity_;
    Quantity  remainingQuantity_;
};

using OrderPointer = std::shared_ptr<Order>;

// A std::list rather than a vector or deque. Erasing from a list leaves every
// other iterator valid, and the cancellation index below relies on exactly that.
using OrderPointers = std::list<OrderPointer>;

// ---------------------------------------------------------------------------
// Order modification request
// ---------------------------------------------------------------------------

class OrderModify
{
public:
    OrderModify(OrderId id, Side side, Price price, Quantity quantity)
        : id_{ id }, side_{ side }, price_{ price }, quantity_{ quantity } {
    }

    OrderId  GetOrderId() const { return id_; }
    Side     GetSide() const { return side_; }
    Price    GetPrice() const { return price_; }
    Quantity GetQuantity() const { return quantity_; }

    // A modification does not say what type of order it is; the book reads the
    // type off the order being replaced and passes it in here.
    OrderPointer ToOrderPointer(OrderType type) const
    {
        return std::make_shared<Order>(type, id_, side_, price_, quantity_);
    }

private:
    OrderId  id_;
    Side     side_;
    Price    price_;
    Quantity quantity_;
};

// ---------------------------------------------------------------------------
// Trades
// ---------------------------------------------------------------------------

struct TradeInfo
{
    OrderId  orderId;
    Price    price;
    Quantity quantity;
};

// A trade records both sides of the match. Each side carries its own limit
// price, not a single execution price, so you can see how much price
// improvement the aggressor got. (The roadmap has an item to add a proper
// execution price, which is what most consumers would actually want.)
//
// Careful with brace initialisation of TradeInfo: the field order is
// {orderId, price, quantity}. Getting this wrong swaps price and quantity
// without any compiler complaint, because both are plain integers.
class Trade
{
public:
    Trade(const TradeInfo& bidTrade, const TradeInfo& askTrade)
        : bidTrade_{ bidTrade }, askTrade_{ askTrade } {
    }

    const TradeInfo& GetBidTrade() const { return bidTrade_; }
    const TradeInfo& GetAskTrade() const { return askTrade_; }

private:
    TradeInfo bidTrade_;
    TradeInfo askTrade_;
};

using Trades = std::vector<Trade>;

// ---------------------------------------------------------------------------
// Order book
// ---------------------------------------------------------------------------

class Orderbook
{
public:
    // Every public method takes the lock exactly once and then calls a
    // "...Locked" helper. That matters for ModifyOrder, which needs to cancel
    // and add as one step: if it called the public versions it would try to
    // lock a mutex it already holds, which deadlocks.

    Trades AddOrder(OrderPointer order)
    {
        std::scoped_lock lock{ mutex_ };
        return AddOrderLocked(std::move(order));
    }

    void CancelOrder(OrderId orderId)
    {
        std::scoped_lock lock{ mutex_ };
        CancelOrderLocked(orderId);
    }

    // Modifying is cancel-then-add, so the order goes to the back of the queue
    // and loses its time priority. Real venues behave like this too, and it is
    // fairer: otherwise you could grow your position without queuing again.
    Trades ModifyOrder(const OrderModify& modify)
    {
        // Check this before anything is cancelled. If we cancelled first and
        // the replacement were then rejected, a bad modify would delete the
        // user's order. A zero-quantity modify is refused and the original
        // order is left exactly as it was.
        if (modify.GetQuantity() == 0)
            return {};

        std::scoped_lock lock{ mutex_ };

        const auto it = orders_.find(modify.GetOrderId());
        if (it == orders_.end())
            return {};

        const OrderType type = it->second.order->GetOrderType();
        CancelOrderLocked(modify.GetOrderId());
        return AddOrderLocked(modify.ToOrderPointer(type));
    }

    // Number of live (resting) orders.
    std::size_t Size() const
    {
        std::scoped_lock lock{ mutex_ };
        return orders_.size();
    }

    std::optional<Price> BestBid() const
    {
        std::scoped_lock lock{ mutex_ };
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    std::optional<Price> BestAsk() const
    {
        std::scoped_lock lock{ mutex_ };
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    // A snapshot of the total quantity at every price level, best price first.
    // It is returned by value so the caller never holds a reference into the
    // book after the lock has been released.
    OrderbookLevelInfos GetOrderInfos() const
    {
        std::scoped_lock lock{ mutex_ };

        LevelInfos bidInfos, askInfos;
        bidInfos.reserve(bids_.size());
        askInfos.reserve(asks_.size());

        for (const auto& [price, orders] : bids_)
            bidInfos.push_back({ price, LevelQuantity(orders) });
        for (const auto& [price, orders] : asks_)
            askInfos.push_back({ price, LevelQuantity(orders) });

        return OrderbookLevelInfos{ std::move(bidInfos), std::move(askInfos) };
    }

private:
    // What the index remembers about each live order: the order itself, and
    // exactly where it sits in its level's list. Holding the iterator is what
    // makes cancelling cheap, because we never have to search for the order.
    struct OrderEntry
    {
        OrderPointer            order;
        OrderPointers::iterator location;
    };

    // Bids sorted highest first, asks lowest first. That way begin() is the
    // best price on either side and matching never has to hunt for it.
    std::map<Price, OrderPointers, std::greater<Price>> bids_;
    std::map<Price, OrderPointers, std::less<Price>>    asks_;
    std::unordered_map<OrderId, OrderEntry>             orders_;
    mutable std::mutex                                  mutex_;

    static TotalQuantity LevelQuantity(const OrderPointers& orders)
    {
        return std::accumulate(orders.begin(), orders.end(), TotalQuantity{ 0 },
            [](TotalQuantity sum, const OrderPointer& o) { return sum + o->GetRemainingQuantity(); });
    }

    // Would an order on `side` at `price` cross the best price on the other side?
    bool CanMatch(Side side, Price price) const
    {
        if (side == Side::Buy)
        {
            if (asks_.empty()) return false;
            return price >= asks_.begin()->first;
        }
        if (bids_.empty()) return false;
        return price <= bids_.begin()->first;
    }

    // Is there enough liquidity at `price` or better to fill `quantity` in
    // full? This is the check FillOrKill depends on. It walks the opposite side
    // outwards from the best price and stops as soon as the target is covered,
    // so it does not touch more levels than it has to.
    bool CanFullyFill(Side side, Price price, Quantity quantity) const
    {
        if (!CanMatch(side, price))
            return false;

        TotalQuantity available = 0;

        // The same walk works for both sides; only the book and the "is this
        // level still within my limit?" test differ, so both are parameters.
        const auto accumulate = [&](const auto& book, auto withinLimit)
            {
                for (const auto& [levelPrice, orders] : book)
                {
                    if (!withinLimit(levelPrice)) break;
                    available += LevelQuantity(orders);
                    if (available >= quantity) return true;
                }
                return false;
            };

        if (side == Side::Buy)
            return accumulate(asks_, [price](Price p) { return p <= price; });
        return accumulate(bids_, [price](Price p) { return p >= price; });
    }

    Trades MatchOrders()
    {
        Trades trades;

        while (!bids_.empty() && !asks_.empty())
        {
            auto& [bidPrice, bids] = *bids_.begin();
            auto& [askPrice, asks] = *asks_.begin();

            if (bidPrice < askPrice)
                break;   // the book no longer crosses, so we are done

            while (!bids.empty() && !asks.empty())
            {
                // Copy the shared pointers before touching the lists. Once an
                // order is popped, a reference to the front element would point
                // at a node that no longer exists.
                const OrderPointer bid = bids.front();
                const OrderPointer ask = asks.front();

                const Quantity quantity =
                    std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

                bid->Fill(quantity);
                ask->Fill(quantity);

                if (bid->IsFilled())
                {
                    bids.pop_front();
                    orders_.erase(bid->GetOrderId());
                }
                if (ask->IsFilled())
                {
                    asks.pop_front();
                    orders_.erase(ask->GetOrderId());
                }

                trades.push_back(Trade{
                    TradeInfo{bid->GetOrderId(), bid->GetPrice(), quantity},
                    TradeInfo{ask->GetOrderId(), ask->GetPrice(), quantity} });
            }

            // Remove an emptied level by iterator, not by key. The key lives
            // inside the very map node being destroyed, so erasing by key while
            // still holding a reference to it is undefined behaviour. (This was
            // a real bug in the first version.)
            if (bids.empty()) bids_.erase(bids_.begin());
            if (asks.empty()) asks_.erase(asks_.begin());
        }

        // A FillAndKill order that matched only partly must not be left resting.
        // It can only ever be the front order of the best level on its side:
        // if it crossed, nothing at or beyond its own price was left on its own
        // side, so it must have become the best level. That lets us check two
        // fronts rather than scan the whole book.
        if (!bids_.empty())
        {
            const auto& order = bids_.begin()->second.front();
            if (order->GetOrderType() == OrderType::FillAndKill)
                CancelOrderLocked(order->GetOrderId());
        }
        if (!asks_.empty())
        {
            const auto& order = asks_.begin()->second.front();
            if (order->GetOrderType() == OrderType::FillAndKill)
                CancelOrderLocked(order->GetOrderId());
        }

        return trades;
    }

    Trades AddOrderLocked(OrderPointer order)
    {
        // Basic validation first, before the order is changed in any way.
        // A null pointer, a repeated id and a zero quantity are all refused.
        // A zero-quantity order would otherwise rest with nothing to trade and
        // later "trade" zero lots, so it is stopped at the door.
        if (!order || order->GetInitialQuantity() == 0 || orders_.contains(order->GetOrderId()))
            return {};

        if (order->GetOrderType() == OrderType::Market)
        {
            // Price the order at the worst level on the opposite side so it is
            // able to sweep everything. If that side is empty there is nothing
            // to trade against and the order is rejected.
            if (order->GetSide() == Side::Buy && !asks_.empty())
                order->ConvertMarketToLimit(asks_.rbegin()->first);
            else if (order->GetSide() == Side::Sell && !bids_.empty())
                order->ConvertMarketToLimit(bids_.rbegin()->first);
            else
                return {};
        }

        if (order->GetOrderType() == OrderType::FillAndKill &&
            !CanMatch(order->GetSide(), order->GetPrice()))
            return {};

        if (order->GetOrderType() == OrderType::FillOrKill &&
            !CanFullyFill(order->GetSide(), order->GetPrice(), order->GetRemainingQuantity()))
            return {};

        // Join the back of the queue at this price, and remember where we are.
        OrderPointers::iterator location;
        if (order->GetSide() == Side::Buy)
        {
            auto& level = bids_[order->GetPrice()];
            level.push_back(order);
            location = std::prev(level.end());
        }
        else
        {
            auto& level = asks_[order->GetPrice()];
            level.push_back(order);
            location = std::prev(level.end());
        }

        orders_.insert({ order->GetOrderId(), OrderEntry{order, location} });
        return MatchOrders();
    }

    void CancelOrderLocked(OrderId orderId)
    {
        const auto it = orders_.find(orderId);
        if (it == orders_.end())
            return;   // unknown or already filled: nothing to do, and not an error

        // Take a copy of the entry before erasing it from the index, since the
        // iterator and pointer we still need live inside it.
        const OrderEntry entry = it->second;
        orders_.erase(it);

        const Price price = entry.order->GetPrice();

        if (entry.order->GetSide() == Side::Sell)
        {
            auto& level = asks_.at(price);
            level.erase(entry.location);
            if (level.empty()) asks_.erase(price);
        }
        else
        {
            auto& level = bids_.at(price);
            level.erase(entry.location);
            if (level.empty()) bids_.erase(price);
        }
    }
};

// ---------------------------------------------------------------------------
// Small demonstration
// ---------------------------------------------------------------------------
// This is a smoke test, not a test suite: it shows the main paths working.
// The real checking lives in tests.cpp.

namespace
{
    void Check(bool condition, const char* description)
    {
        std::cout << (condition ? "[ok]   " : "[FAIL] ") << description << '\n';
        if (!condition) std::exit(EXIT_FAILURE);
    }

    OrderPointer Limit(OrderType type, OrderId id, Side side, Price price, Quantity qty)
    {
        return std::make_shared<Order>(type, id, side, price, qty);
    }
}

int main()
{
    Orderbook book;

    // Two orders that do not cross, so both simply rest.
    book.AddOrder(Limit(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10));
    book.AddOrder(Limit(OrderType::GoodTillCancel, 2, Side::Sell, 105, 5));
    Check(book.Size() == 2, "two resting orders");
    Check(book.BestBid() == 100 && book.BestAsk() == 105, "best bid 100, best ask 105");

    // An aggressive sell at 100 takes 4 lots from order 1 and leaves 6.
    auto trades = book.AddOrder(Limit(OrderType::GoodTillCancel, 3, Side::Sell, 100, 4));
    Check(trades.size() == 1 && trades[0].GetBidTrade().quantity == 4, "partial fill of 4 against order 1");
    Check(book.GetOrderInfos().GetBids().front().quantity == 6, "6 left at the best bid");

    // A market buy for 8 can only find 5 on offer. It takes them, and the
    // other 3 are cancelled rather than left sitting on the book.
    trades = book.AddOrder(std::make_shared<Order>(4, Side::Buy, 8));
    Check(trades.size() == 1 && trades[0].GetAskTrade().quantity == 5, "market order filled for 5");
    Check(book.Size() == 1, "market remainder did not rest");

    // Fill-or-kill with nothing to fill against is turned away.
    trades = book.AddOrder(Limit(OrderType::FillOrKill, 5, Side::Buy, 105, 1));
    Check(trades.empty() && book.Size() == 1, "fill-or-kill rejected");

    // Zero-quantity orders are refused, and so is modifying an order down to
    // zero (the original is left alone).
    trades = book.AddOrder(Limit(OrderType::GoodTillCancel, 6, Side::Buy, 98, 0));
    Check(trades.empty() && book.Size() == 1, "zero-quantity order rejected");
    book.ModifyOrder(OrderModify{ 1, Side::Buy, 99, 0 });
    Check(book.Size() == 1 && book.BestBid() == 100, "zero-quantity modify ignored");

    // Reprice order 1 (which sends it to the back of its queue), then cancel it.
    book.ModifyOrder(OrderModify{ 1, Side::Buy, 99, 6 });
    Check(book.BestBid() == 99, "order 1 repriced to 99");
    book.CancelOrder(1);
    Check(book.Size() == 0, "book empty after cancel");

    return 0;
}
