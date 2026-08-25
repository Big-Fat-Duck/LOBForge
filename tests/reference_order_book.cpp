#include "reference_order_book.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <type_traits>

namespace lob::test {

std::vector<Event> ReferenceOrderBook::process(const Command &command) {
  return std::visit(
      [this](const auto &value) -> std::vector<Event> {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, NewOrder>) {
          return process_new(value);
        } else if constexpr (std::is_same_v<Value, CancelOrder>) {
          return process_cancel(value);
        } else if constexpr (std::is_same_v<Value, ReduceOrder>) {
          return process_reduce(value);
        } else {
          return process_replace(value);
        }
      },
      command);
}

Quantity ReferenceOrderBook::aggregate(const Side side, const Price price,
                                       const std::optional<OrderId> excluding) const {
  Quantity result = 0;
  for (const auto &order : orders_) {
    if (order.side == side && order.price == price &&
        (!excluding.has_value() || order.order_id != *excluding)) {
      result += order.quantity;
    }
  }
  return result;
}

Quantity ReferenceOrderBook::executable(const NewOrder &command) const {
  std::vector<const Order *> eligible;
  for (const auto &order : orders_) {
    const bool crosses = command.type == OrderType::Market ||
                         (command.side == Side::Buy && order.price <= *command.limit_price) ||
                         (command.side == Side::Sell && order.price >= *command.limit_price);
    if (order.side != command.side && crosses) {
      eligible.push_back(&order);
    }
  }
  std::sort(eligible.begin(), eligible.end(), [&command](const Order *left, const Order *right) {
    if (left->price != right->price) {
      return command.side == Side::Buy ? left->price < right->price : left->price > right->price;
    }
    return left->priority_sequence < right->priority_sequence;
  });
  Quantity result = 0;
  for (const auto *order : eligible) {
    const Quantity missing = command.quantity - result;
    result += std::min(missing, order->quantity);
    if (result == command.quantity) {
      break;
    }
  }
  return result;
}

std::optional<RejectReason>
ReferenceOrderBook::validate(const NewOrder &command, const Quantity rest_quantity,
                             const std::optional<OrderId> replacing) const {
  const auto existing = find(command.order_id);
  if (existing != orders_.end() && (!replacing.has_value() || existing->order_id != *replacing)) {
    return RejectReason::DuplicateOrderId;
  }
  if (command.quantity == 0) {
    return RejectReason::ZeroQuantity;
  }
  if (command.type == OrderType::Limit) {
    if (!command.limit_price.has_value() || *command.limit_price <= 0) {
      return RejectReason::InvalidPrice;
    }
  } else {
    if (command.limit_price.has_value()) {
      return RejectReason::InvalidPrice;
    }
    if (command.time_in_force != TimeInForce::IOC && command.time_in_force != TimeInForce::FOK) {
      return RejectReason::InvalidOrderTypeTimeInForce;
    }
  }
  if (command.time_in_force == TimeInForce::PostOnly && command.type != OrderType::Limit) {
    return RejectReason::InvalidOrderTypeTimeInForce;
  }
  if (command.time_in_force == TimeInForce::FOK && executable(command) < command.quantity) {
    return RejectReason::CannotFullyFill;
  }
  if (command.time_in_force == TimeInForce::PostOnly) {
    const auto opposite_best = command.side == Side::Buy ? best_ask() : best_bid();
    const bool crosses = opposite_best.has_value() &&
                         (command.side == Side::Buy ? *opposite_best <= *command.limit_price
                                                    : *opposite_best >= *command.limit_price);
    if (crosses) {
      return RejectReason::WouldCross;
    }
  }
  if (rest_quantity != 0) {
    const Quantity current = aggregate(command.side, *command.limit_price, replacing);
    if (rest_quantity > std::numeric_limits<Quantity>::max() - current) {
      return RejectReason::AggregateOverflow;
    }
  }
  return std::nullopt;
}

std::vector<Event> ReferenceOrderBook::process_new(const NewOrder &command) {
  Quantity rest_quantity = 0;
  if (command.type == OrderType::Limit && command.limit_price.has_value() &&
      (command.time_in_force == TimeInForce::GTC ||
       command.time_in_force == TimeInForce::PostOnly)) {
    const Quantity available = executable(command);
    rest_quantity = command.time_in_force == TimeInForce::PostOnly
                        ? command.quantity
                        : command.quantity - std::min(command.quantity, available);
  }
  if (const auto rejection = validate(command, rest_quantity, std::nullopt)) {
    return {Rejected{event_sequence(), command.order_id, *rejection}};
  }
  std::vector<Event> events{Accepted{event_sequence(), command.order_id, command.quantity}};
  Quantity remaining = command.quantity;
  if (command.time_in_force != TimeInForce::PostOnly) {
    match(command, remaining, events);
  }
  if (remaining != 0) {
    if (command.type == OrderType::Limit && (command.time_in_force == TimeInForce::GTC ||
                                             command.time_in_force == TimeInForce::PostOnly)) {
      rest(command, remaining, events);
    } else {
      events.emplace_back(Expired{event_sequence(), command.order_id, remaining,
                                  command.type == OrderType::Market
                                      ? ExpireReason::MarketRemainder
                                      : ExpireReason::ImmediateOrCancel});
    }
  }
  return events;
}

void ReferenceOrderBook::match(const NewOrder &command, Quantity &remaining,
                               std::vector<Event> &events) {
  while (remaining != 0) {
    auto maker = orders_.end();
    for (auto candidate = orders_.begin(); candidate != orders_.end(); ++candidate) {
      if (candidate->side == command.side) {
        continue;
      }
      const bool crosses =
          command.type == OrderType::Market ||
          (command.side == Side::Buy && candidate->price <= *command.limit_price) ||
          (command.side == Side::Sell && candidate->price >= *command.limit_price);
      if (!crosses) {
        continue;
      }
      const bool better = maker == orders_.end() ||
                          (command.side == Side::Buy && candidate->price < maker->price) ||
                          (command.side == Side::Sell && candidate->price > maker->price) ||
                          (candidate->price == maker->price &&
                           candidate->priority_sequence < maker->priority_sequence);
      if (better) {
        maker = candidate;
      }
    }
    if (maker == orders_.end()) {
      break;
    }
    const Quantity traded = std::min(remaining, maker->quantity);
    remaining -= traded;
    maker->quantity -= traded;
    events.emplace_back(Trade{event_sequence(), maker->order_id, command.order_id, maker->price,
                              traded, command.side});
    if (maker->quantity == 0) {
      orders_.erase(maker);
    }
  }
}

void ReferenceOrderBook::rest(const NewOrder &command, const Quantity remaining,
                              std::vector<Event> &events) {
  orders_.push_back(Order{command.order_id, command.side, *command.limit_price, remaining,
                          priority_sequence(), command.timestamp});
  events.emplace_back(Rested{event_sequence(), command.order_id, *command.limit_price, remaining});
}

std::vector<Event> ReferenceOrderBook::process_cancel(const CancelOrder &command) {
  const auto order = find(command.order_id);
  if (order == orders_.end()) {
    return {Rejected{event_sequence(), command.order_id, RejectReason::UnknownOrderId}};
  }
  const Quantity quantity = order->quantity;
  orders_.erase(order);
  return {Cancelled{event_sequence(), command.order_id, quantity}};
}

std::vector<Event> ReferenceOrderBook::process_reduce(const ReduceOrder &command) {
  const auto order = find(command.order_id);
  if (order == orders_.end()) {
    return {Rejected{event_sequence(), command.order_id, RejectReason::UnknownOrderId}};
  }
  if (command.reduce_by == 0 || command.reduce_by >= order->quantity) {
    return {Rejected{event_sequence(), command.order_id, RejectReason::InvalidReduction}};
  }
  const Quantity old_quantity = order->quantity;
  order->quantity -= command.reduce_by;
  return {Reduced{event_sequence(), command.order_id, old_quantity, order->quantity}};
}

std::vector<Event> ReferenceOrderBook::process_replace(const ReplaceOrder &command) {
  const auto order = find(command.order_id);
  if (order == orders_.end()) {
    return {Rejected{event_sequence(), command.order_id, RejectReason::UnknownOrderId}};
  }
  const Price old_price = order->price;
  const Quantity old_quantity = order->quantity;
  if (command.new_quantity == 0 || command.new_price <= 0) {
    return {Rejected{event_sequence(), command.order_id,
                     command.new_quantity == 0 ? RejectReason::ZeroQuantity
                                               : RejectReason::InvalidPrice}};
  }
  if (command.new_price == old_price && command.new_quantity <= old_quantity) {
    order->quantity = command.new_quantity;
    return {Replaced{event_sequence(), command.order_id, old_price, old_quantity, command.new_price,
                     command.new_quantity, true}};
  }

  const NewOrder replacement{command.order_id, order->side,       OrderType::Limit,
                             TimeInForce::GTC, command.new_price, command.new_quantity,
                             command.timestamp};
  const Quantity available = executable(replacement);
  const Quantity rest_quantity = command.new_quantity - std::min(command.new_quantity, available);
  if (const auto rejection = validate(replacement, rest_quantity, command.order_id)) {
    return {Rejected{event_sequence(), command.order_id, *rejection}};
  }
  orders_.erase(order);
  std::vector<Event> events{Replaced{event_sequence(), command.order_id, old_price, old_quantity,
                                     command.new_price, command.new_quantity, false}};
  Quantity remaining = command.new_quantity;
  match(replacement, remaining, events);
  if (remaining != 0) {
    rest(replacement, remaining, events);
  }
  return events;
}

std::optional<Price> ReferenceOrderBook::best_bid() const {
  std::optional<Price> result;
  for (const auto &order : orders_) {
    if (order.side == Side::Buy && (!result.has_value() || order.price > *result)) {
      result = order.price;
    }
  }
  return result;
}

std::optional<Price> ReferenceOrderBook::best_ask() const {
  std::optional<Price> result;
  for (const auto &order : orders_) {
    if (order.side == Side::Sell && (!result.has_value() || order.price < *result)) {
      result = order.price;
    }
  }
  return result;
}

BookSnapshot ReferenceOrderBook::snapshot() const {
  std::map<Price, DepthLevel, std::greater<Price>> bids;
  std::map<Price, DepthLevel, std::less<Price>> asks;
  for (const auto &order : orders_) {
    auto &level = order.side == Side::Buy ? bids[order.price] : asks[order.price];
    level.price = order.price;
    level.quantity += order.quantity;
    ++level.order_count;
  }
  BookSnapshot result;
  for (const auto &[price, level] : bids) {
    (void)price;
    result.bids.push_back(level);
  }
  for (const auto &[price, level] : asks) {
    (void)price;
    result.asks.push_back(level);
  }
  return result;
}

std::vector<OrderView> ReferenceOrderBook::active_orders() const {
  std::vector<OrderView> result;
  for (const auto &order : orders_) {
    std::size_t position = 0;
    for (const auto &candidate : orders_) {
      if (candidate.side == order.side && candidate.price == order.price &&
          candidate.priority_sequence < order.priority_sequence) {
        ++position;
      }
    }
    result.push_back(OrderView{order.order_id, order.side, order.price, order.quantity,
                               order.priority_sequence, position});
  }
  std::sort(result.begin(), result.end(), [](const OrderView &left, const OrderView &right) {
    return left.order_id < right.order_id;
  });
  return result;
}

} // namespace lob::test
