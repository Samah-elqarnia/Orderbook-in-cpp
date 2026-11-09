
#include "Orderbook.h"

#include <numeric>
#include <chrono>
#include <ctime>

//the actual functions 

bool Orderbook::CanMatch(Side side, Price price) const 
       {
        if (side == Side::Buy){
            if ( asks_.empty())
            return false;
            const auto& [bestAsk, _]= *asks_.begin();
            return price >= bestAsk;
        }
        else{
            if(bids_.empty())
            return false;

            const auto& [bestBid, _]= *bids_.begin();
            return price <= bestBid;
        }
       }

Trades Orderbook::MatchOrders( )
       {
        Trades trades;
        trades.reserve(orders_.size());

        while (true)
        {
            if ( bids_.empty() || asks_.empty())
            break; 

            auto& [bidPrice , bids ]= *bids_.begin();
            auto& [askPrice , asks]= *asks_.begin();

            if(bidPrice < askPrice)
               break;


            while (bids.size() && asks.size()){
                auto& bid= bids.front();
                auto& ask = asks.front();

                Quantity quantity = std :: min (bid -> GetRemainingQuantity() , ask->GetRemainingQuantity());

                bid->Fill(quantity);
                ask->Fill(quantity);

                if(bid->IsFilled())
                {
                    bids.pop_front();
                    orders_.erase(bid->GetOrderId());
                }
                if( ask->IsFilled())
                {
                    asks.pop_front();
                    orders_.erase(ask->GetOrderId());
                }
                if (bids.empty())
                bids_.erase(bidPrice);
                
                trades.push_back(Trade{ 
                    TradeInfo{ bid->GetOrderId(), bid->GetPrice(), quantity},
                    TradeInfo{ ask->GetOrderId(), ask->GetPrice(), quantity}
                });
            }
        }
        if (!bids_.empty())
        {
            auto& [_,bids]= *bids_.begin();
            auto& order= bids.front();
            if( order->GetOrderType()== OrderType:: FillAndKill)
              CancelOrder(order->GetOrderId());
        }


        if (!asks_.empty())
        {
            auto& [_,asks]= *asks_.begin();
            auto& order= asks.front();
            if( order->GetOrderType()== OrderType:: FillAndKill)
              CancelOrder(order->GetOrderId());
        }

        return trades;
       }


Trades Orderbook::AddOrder(OrderPointer order)
   {
    
    if (orders_.contains(order->GetOrderId()))
        return { };

    if (orders->GetOrderType() == OrderType::Market)
    {
        if(order->GetSide()==Side::Buy && !asks_.empty())
        {
            const auto& [worstAsk, _]= *asks_.rbegin();
            order->ToGoodTillCancel(worstAsk);
        }


        if(order->GetSide()==Side::Sell && !bids_.empty())
        {
            const auto& [worstBid, _]= *bids_.rbegin();
            order->ToGoodTillCancel(worstBid);
        }
    }    
    if (order -> GetOrderType()== OrderType:: FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
        return { };    

   OrderPointers::iterator iterator;

   if (order->GetSide()== Side::Buy)
   {
    auto& orders =bids_[order->GetPrice()];
    orders.push_back(order);
    iterator = std::next(orders.begin(), static_cast<std::ptrdiff_t>(orders.size() - 1));

   }
   else
   {
   auto& orders = asks_[order->GetPrice()];
   orders.push_back(order);
   iterator = std::next(orders.begin(), static_cast<std::ptrdiff_t>(orders.size() - 1));
   }     

   orders_.insert({ order->GetOrderId(), OrderEntry{order, iterator}});
   return MatchOrders();

   }

void Orderbook::CancelOrder(OrderId orderId)
   {
    auto it = orders_.find(orderId);
    if (it == orders_.end())
        return;

    OrderPointer order = it->second.order_;
    OrderPointers::iterator iterator = it->second.location_;
    orders_.erase(it);

    auto price = order->GetPrice();
    if (order->GetSide() == Side::Buy)
    {
        auto bidIt = bids_.find(price);
        if (bidIt == bids_.end())
            return;

        auto& orders = bidIt->second;
        orders.erase(iterator);
        if (orders.empty())
            bids_.erase(price);
    }
    else
    {
        auto askIt = asks_.find(price);
        if (askIt == asks_.end())
            return;

        auto& orders = askIt->second;
        orders.erase(iterator);
        if (orders.empty())
            asks_.erase(price);
    }
   }

Trades Orderbook::ModifyOrder (OrderModify order)
   {

    if(!orders_.contains(order.GetOrderId()))
      return { };
    
    const auto& [existingOrder , _]= orders_.at(order.GetOrderId());
    CancelOrder(order.GetOrderId());
    return AddOrder(order.ToOrderPointer(existingOrder->GetOrderType()));
   }
   
   
std::size_t Orderbook::Size() const 
{ 
    return orders_.size();
 }


OrderbookLevelInfos Orderbook::GetOrderInfos() const 
   {
    LevelInfos bidInfos, askInfos;
    bidInfos.reserve(orders_.size());
    askInfos.reserve(orders_.size());

    auto CreatelevelInfos = [](Price price, const OrderPointers& orders)
    {
        Quantity total = 0;
        for (const auto& order : orders)
            total += order->GetRemainingQuantity();
        return LevelInfo{price, total};
    };

    for (const auto& [price, orders] : bids_)
        bidInfos.push_back(CreatelevelInfos(price, orders));

    for (const auto& [price, orders] : asks_)
        askInfos.push_back(CreatelevelInfos(price, orders));

    return OrderbookLevelInfos{bidInfos, askInfos};
   }