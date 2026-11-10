
#include "Orderbook.h"

#include <numeric>
#include <chrono>
#include <ctime>

//the actual functions 

void Orderbook:: PruneGoodForDayOrders()
{

    using namespace std:: chrono;
    const auto end= hours(16);

    while(true)
    {
        const auto now= system_clock::now();
        const auto now_c= system_clock::to_time_t(now);
        std::tm now_parts;
        localtime_s(&now_parts, &now_c);

        if (now_parts.tm_hour >= end.count())
           now_parts.tm_nday += 1;

        now_parts.tm_hour= end.count();
        now_parts.tm_min = 0;
        now_parts.tm_sec = 0;  

        auto next= system_clock:: from_time_t(mktime(&now_parts));
        auto till= next - now+ milliseconds(100);
        {
            std::unique_lock ordersLock{ ordersMutex_};

            if( shutdown_.load(std::memory_order_acquire ) ||
				shutdownConditionVariable_.wait_for(ordersLock, till) == std::cv_status::no_timeout)
				return;
        }
        OrderIds orderIds;

        {
           std::scoped_lock ordersLock{ ordersMutex_ };

			for (const auto& [_, entry] : orders_)
			{
				const auto& [order, _] = entry;

				if (order->GetOrderType() != OrderType::GoodForDay)
					continue;

				orderIds.push_back(order->GetOrderId());
			}
		}

		CancelOrders(orderIds); 
        }
    }


void Orderbook::CancelOrders(OrderIds orderIds)
{
	std::scoped_lock ordersLock{ ordersMutex_ };

	for (const auto& orderId : orderIds)
		CancelOrderInternal(orderId);
}


void Orderbook::CancelOrderInternal(OrderId orderId)
{
   if (!orders_.contains(orderId))
      return;
    
   const auto [order,iterator]= orders_.at(orderId);
   orders_.erase(orderId);
   
   if(order->GetSide()==Side::Sell)
   {
    auto price = order->GetPrice();
    auto& orders = asks_.at(price);
		orders.erase(iterator);
		if (orders.empty())
			asks_.erase(price);
   }
   else{
    auto price = order->GetPrice();
    auto& orders = bids_.at(price);
		orders.erase(iterator);
		if (orders.empty())
			bids_.erase(price);
   }

   OnOrderCancelled(order); 
}


void Orderbook::OnOrderCancelled(OrderPointer order)
{
	UpdateLevelData(order->GetPrice(), order->GetRemainingQuantity(), LevelData::Action::Remove);
}

void Orderbook::OnOrderAdded(OrderPointer order)
{
    UpdateLevelData(order->GetPrice(), order->GetInitialQuantity(), LevelData::Action::Add);
}

void Orderbook::OnOrderMatched(Price price, Quantity quantity, bool isfullyfilled)
{
    UpdateLevelData(price, quantity, isFullyFilled ? LevelData::Action::Remove : LevelData::Action::Match);
}

void Orderbook::UpdateLevelData(Price price, Quantity quantity, LevelData::Action action)
{
    auto& data = data_[price];

    data.count_ += action == LevelData::Action::Remove ? -1 : action == LevelData::Action::Add ? 1 : 0 ;
    if (action==LevelData::Action::Remove || action == LevelData::Action::Match )
    {
        data.quantity_ -= quantity;
    }
    else{
        data.quantity_ += quantity;
    }
    if (data.count_ == 0)
        data_.erase(price);
}


bool Orderbook::CanFullyFill(Side side , Price price, Quantity quantity) const {
    if(!CanMatch(side, price))
       return false;

    std::optional<Price> threshold;
    
    if (side == Side::Buy)
    {
        const auto [askPrice,_]= *asks_.begin();
        threshold = askPrice;
    }
    else{
        const auto [bidPrice, _] = *bids_.begin();
        threshold= bidPrice;
    }

    for(const auto& [levelPrice,levelData] : data_)
    {
        if(threshold.has-value()&&
           (side == Side::Buy && threshold.value() > levelPrice)||
           (side == Side::Sell && threshold.value()< levelPrice) 
        )
          continue;
        
        if(quantity <= levelData.quantity_)
          return true;
        
        quantity -= levelData.quantity_;
    }

    return false;
}

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


            while (bids.empty() && asks.empty()){
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



Orderbook::Orderbook() : ordersPrunethread_ {  [this] { PruneGoodForDayOrders(); }} { }

Orderbook::~Orderbook()
{
  shutdown_.store(true, std::memory_order_release);
  shutdownConditionVariable_.notify_one();
  ordersPruneThread_.join();
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