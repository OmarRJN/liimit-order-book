// orderbook.cpp
// A price-time priority limit order book.
//
// Matching rules: the best bid is matched against the best ask for as long as
// they cross. Within a price level, orders are filled in arrival order (FIFO).
//
// Supported order types:
//   GoodTillCancel : rests on the book until filled or cancelled.
//   FillAndKill    : matches whatever it can immediately; any remainder is cancelled.
//   FillOrKill     : executes in full immediately, or not at all.
//   Market         : takes liquidity at any price; any unfilled remainder is cancelled.
//
// Build: g++ -std=c++20 -Wall -Wextra -O2 orderbook.cpp -o orderbook

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

using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

// Market orders carry no price of their own, so we use a sentinel value.
constexpr Price InvalidPrice = std::numeric_limits<Price>::min();

// ---------------------------------------------------------------------------
// Aggregated view of the book (price level -> total resting quantity)
// ---------------------------------------------------------------------------

struct LevelInfo
{
    Price    price;
    Quantity quantity;
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
    // Limit-style order (GoodTillCancel, FillAndKill, FillOrKill).
    Order(OrderType type, OrderId id, Side side, Price price, Quantity quantity)
        : type_{ type }, id_{ id }, side_{ side }, price_{ price },
        initialQuantity_{ quantity }, remainingQuantity_{ quantity } {
    }

    // Market order: no price until the book assigns one.
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
        if (quantity > remainingQuantity_)
            throw std::logic_error("Order cannot be filled for more than its remaining quantity");
        remainingQuantity_ -= quantity;
    }

    // Gives a market order a concrete limit price (the worst price currently on
    // the opposite side) and makes it FillAndKill, so it sweeps the book but
    // never rests.
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
using OrderPointers = std::list<OrderPointer>;   // list: iterators stay valid on erase

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

    // The order type is not part of a modification; the book supplies it from
    // the order being replaced.
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

// One trade records both sides. Each side reports its own limit price, which
// lets a consumer see the price improvement the aggressor received.
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
    // Public operations take the lock once, then call the *Locked helpers so
    // that composite operations (such as ModifyOrder) never lock recursively.

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

    // A modification is a cancel followed by a fresh add, so the order loses
    // its time priority. This is the usual behaviour on real exchanges.
    Trades ModifyOrder(const OrderModify& modify)
    {
        std::scoped_lock lock{ mutex_ };

        const auto it = orders_.find(modify.GetOrderId());
        if (it == orders_.end())
            return {};

        const OrderType type = it->second.order->GetOrderType();
        CancelOrderLocked(modify.GetOrderId());
        return AddOrderLocked(modify.ToOrderPointer(type));
    }

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

    // Snapshot of aggregated quantity at every price level, best price first.
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
    struct OrderEntry
    {
        OrderPointer           order;
        OrderPointers::iterator location;   // position inside its price level
    };

    // Bids are sorted highest first, asks lowest first, so begin() is always
    // the best price on either side.
    std::map<Price, OrderPointers, std::greater<Price>> bids_;
    std::map<Price, OrderPointers, std::less<Price>>    asks_;
    std::unordered_map<OrderId, OrderEntry>             orders_;
    mutable std::mutex                                  mutex_;

    static Quantity LevelQuantity(const OrderPointers& orders)
    {
        return std::accumulate(orders.begin(), orders.end(), Quantity{ 0 },
            [](Quantity sum, const OrderPointer& o) { return sum + o->GetRemainingQuantity(); });
    }

    // Would an order on `side` at `price` cross the opposite best price?
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

    // Is there enough liquidity at or better than `price` to fill `quantity`
    // in full? Needed for FillOrKill. Walks the opposite side from the best
    // price outwards and stops as soon as the quantity is covered.
    bool CanFullyFill(Side side, Price price, Quantity quantity) const
    {
        if (!CanMatch(side, price))
            return false;

        Quantity available = 0;

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
                break;   // book no longer crosses

            while (!bids.empty() && !asks.empty())
            {
                // Copy the pointers: popping below would otherwise leave us
                // holding references to removed list nodes.
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

            // Remove exhausted levels by iterator rather than by key: the key
            // is a reference into the very node being destroyed.
            if (bids.empty()) bids_.erase(bids_.begin());
            if (asks.empty()) asks_.erase(asks_.begin());
        }

        // A FillAndKill order that was only partially matched must not rest.
        // Any such remainder can only be at the front of the best level.
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
        if (!order || orders_.contains(order->GetOrderId()))
            return {};

        if (order->GetOrderType() == OrderType::Market)
        {
            // Price the order at the worst level on the opposite side so that
            // it is able to sweep the whole book. No liquidity, no order.
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
            return;

        // Copy before erasing: the map entry owns the data we still need.
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

    // Two non-crossing GTC orders rest on the book.
    book.AddOrder(Limit(OrderType::GoodTillCancel, 1, Side::Buy, 100, 10));
    book.AddOrder(Limit(OrderType::GoodTillCancel, 2, Side::Sell, 105, 5));
    Check(book.Size() == 2, "two resting orders");
    Check(book.BestBid() == 100 && book.BestAsk() == 105, "best bid 100, best ask 105");

    // An aggressive sell partially fills order 1.
    auto trades = book.AddOrder(Limit(OrderType::GoodTillCancel, 3, Side::Sell, 100, 4));
    Check(trades.size() == 1 && trades[0].GetBidTrade().quantity == 4, "partial fill of 4 against order 1");
    Check(book.GetOrderInfos().GetBids().front().quantity == 6, "6 left at the best bid");

    // A market buy for 8 exhausts the 5 on offer; the remaining 3 are cancelled.
    trades = book.AddOrder(std::make_shared<Order>(4, Side::Buy, 8));
    Check(trades.size() == 1 && trades[0].GetAskTrade().quantity == 5, "market order filled for 5");
    Check(book.Size() == 1, "market remainder did not rest");

    // A fill-or-kill with no available liquidity is rejected outright.
    trades = book.AddOrder(Limit(OrderType::FillOrKill, 5, Side::Buy, 105, 1));
    Check(trades.empty() && book.Size() == 1, "fill-or-kill rejected");

    // Modify then cancel.
    book.ModifyOrder(OrderModify{ 1, Side::Buy, 99, 6 });
    Check(book.BestBid() == 99, "order 1 repriced to 99");
    book.CancelOrder(1);
    Check(book.Size() == 0, "book empty after cancel");

    return 0;
}









































int main() {
    Bids bids;
    Asks asks;
    uint64_t nextOrderId = 1;
    uint64_t timestamp = 0;

    std::string line;
    std::cout << "Enter orders as: side price quantity (e.g. buy 101.50 100). Blank line to stop.\n";

    while (std::getline(std::cin, line)) {
        if (line.empty()) break;

        std::istringstream iss(line);
        std::string sideStr;
        double price;
        uint64_t quantity;

        if (!(iss >> sideStr >> price >> quantity)) {
            std::cout << "Could not parse line, skipping.\n";
            continue;
        }

        Side side = (sideStr == "buy" || sideStr == "Buy") ? Side::Buy : Side::Sell;
        Order order(nextOrderId++, side, OrderType::Limit, price, quantity, timestamp++);

        addOrder(bids, asks, order);

        auto bb = BestPrice(bids);
        auto ba = BestPrice(asks);
        std::cout << "Best bid: " << (bb ? std::to_string(*bb) : "none")
            << " | Best ask: " << (ba ? std::to_string(*ba) : "none") << "\n";
    }

    return 0;
}
	



	