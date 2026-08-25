#include "lob/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace lob {
namespace {

constexpr Quantity kMaxQuantity = std::numeric_limits<Quantity>::max();
constexpr SequenceNumber kMaxSequence = std::numeric_limits<SequenceNumber>::max();

[[nodiscard]] bool checked_add(const Quantity left, const Quantity right,
                               Quantity &result) noexcept {
  if (right > kMaxQuantity - left) {
    return false;
  }
  result = left + right;
  return true;
}

} // namespace

SequenceNumber event_sequence(const Event &event) noexcept {
  return std::visit([](const auto &value) { return value.sequence; }, event);
}

std::vector<Event> OrderBook::process(const Command &command) {
  auto events = std::visit(
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
#ifndef NDEBUG
  std::string error;
  assert(check_invariants(&error));
#endif
  return events;
}

SequenceNumber OrderBook::take_event_sequence() {
  if (next_event_sequence_ == kMaxSequence) {
    throw std::overflow_error("event sequence exhausted");
  }
  return next_event_sequence_++;
}

SequenceNumber OrderBook::take_priority_sequence() {
  if (next_priority_sequence_ == kMaxSequence) {
    throw std::overflow_error("priority sequence exhausted");
  }
  return next_priority_sequence_++;
}

std::optional<RejectReason> OrderBook::validate_new(const NewOrder &command,
                                                    const Quantity rest_quantity,
                                                    const std::optional<OrderId> replacing) const {
  const auto existing = order_index_.find(command.order_id);
  if (existing != order_index_.end() && (!replacing.has_value() || existing->first != *replacing)) {
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
  if (command.time_in_force == TimeInForce::FOK &&
      executable_quantity(command) < command.quantity) {
    return RejectReason::CannotFullyFill;
  }
  if (command.time_in_force == TimeInForce::PostOnly &&
      would_cross(command.side, *command.limit_price)) {
    return RejectReason::WouldCross;
  }
  if (rest_quantity != 0 &&
      !can_add_to_level(command.side, *command.limit_price, rest_quantity, replacing)) {
    return RejectReason::AggregateOverflow;
  }
  if (next_event_sequence_ == kMaxSequence ||
      (rest_quantity != 0 && next_priority_sequence_ == kMaxSequence)) {
    return RejectReason::SequenceExhausted;
  }
  return std::nullopt;
}

Quantity OrderBook::executable_quantity(const NewOrder &command) const noexcept {
  Quantity available = 0;
  const auto accumulate = [&available](const PriceLevel &level, const Quantity needed) {
    const Quantity missing = needed - available;
    available += std::min(level.aggregate_quantity, missing);
  };

  if (command.side == Side::Buy) {
    for (const auto &[price, level] : asks_) {
      if (command.type == OrderType::Limit && price > *command.limit_price) {
        break;
      }
      accumulate(level, command.quantity);
      if (available == command.quantity) {
        break;
      }
    }
  } else {
    for (const auto &[price, level] : bids_) {
      if (command.type == OrderType::Limit && price < *command.limit_price) {
        break;
      }
      accumulate(level, command.quantity);
      if (available == command.quantity) {
        break;
      }
    }
  }
  return available;
}

bool OrderBook::would_cross(const Side side, const Price price) const noexcept {
  if (side == Side::Buy) {
    return !asks_.empty() && asks_.begin()->first <= price;
  }
  return !bids_.empty() && bids_.begin()->first >= price;
}

Quantity OrderBook::predicted_remainder(const NewOrder &command) const noexcept {
  if (command.type == OrderType::Limit && !command.limit_price.has_value()) {
    return 0;
  }
  if (command.type == OrderType::Market || command.time_in_force == TimeInForce::IOC ||
      command.time_in_force == TimeInForce::FOK) {
    return 0;
  }
  const Quantity executable = executable_quantity(command);
  return command.quantity - std::min(command.quantity, executable);
}

bool OrderBook::can_add_to_level(const Side side, const Price price, const Quantity quantity,
                                 const std::optional<OrderId> replacing) const noexcept {
  Quantity current = quantity_at(side, price);
  if (replacing.has_value()) {
    const auto indexed = order_index_.find(*replacing);
    if (indexed != order_index_.end() && indexed->second.side == side &&
        indexed->second.price == price) {
      current -= indexed->second.order->remaining_quantity;
    }
  }
  Quantity ignored = 0;
  return checked_add(current, quantity, ignored);
}

std::vector<Event> OrderBook::process_new(const NewOrder &command) {
  const Quantity rest_quantity = predicted_remainder(command);
  if (const auto rejection = validate_new(command, rest_quantity, std::nullopt)) {
    return {Rejected{take_event_sequence(), command.order_id, *rejection}};
  }

  std::vector<Event> events;
  events.emplace_back(Accepted{take_event_sequence(), command.order_id, command.quantity});
  Quantity remaining = command.quantity;
  if (command.time_in_force != TimeInForce::PostOnly) {
    match(command, remaining, events);
  }

  if (remaining != 0) {
    if (command.type == OrderType::Limit && (command.time_in_force == TimeInForce::GTC ||
                                             command.time_in_force == TimeInForce::PostOnly)) {
      add_resting(command, remaining, events);
    } else {
      const auto reason = command.type == OrderType::Market ? ExpireReason::MarketRemainder
                                                            : ExpireReason::ImmediateOrCancel;
      events.emplace_back(Expired{take_event_sequence(), command.order_id, remaining, reason});
    }
  }
  return events;
}

void OrderBook::match(const NewOrder command, Quantity &remaining, std::vector<Event> &events) {
  if (command.side == Side::Buy) {
    auto level = asks_.begin();
    while (remaining != 0 && level != asks_.end() &&
           (command.type == OrderType::Market || level->first <= *command.limit_price)) {
      auto &orders = level->second.orders;
      while (remaining != 0 && !orders.empty()) {
        auto maker = orders.begin();
        const Quantity traded = std::min(remaining, maker->remaining_quantity);
        remaining -= traded;
        maker->remaining_quantity -= traded;
        level->second.aggregate_quantity -= traded;
        events.emplace_back(Trade{take_event_sequence(), maker->order_id, command.order_id,
                                  level->first, traded, command.side});
        if (maker->remaining_quantity == 0) {
          order_index_.erase(maker->order_id);
          orders.erase(maker);
        }
      }
      if (orders.empty()) {
        level = asks_.erase(level);
      } else {
        break;
      }
    }
  } else {
    auto level = bids_.begin();
    while (remaining != 0 && level != bids_.end() &&
           (command.type == OrderType::Market || level->first >= *command.limit_price)) {
      auto &orders = level->second.orders;
      while (remaining != 0 && !orders.empty()) {
        auto maker = orders.begin();
        const Quantity traded = std::min(remaining, maker->remaining_quantity);
        remaining -= traded;
        maker->remaining_quantity -= traded;
        level->second.aggregate_quantity -= traded;
        events.emplace_back(Trade{take_event_sequence(), maker->order_id, command.order_id,
                                  level->first, traded, command.side});
        if (maker->remaining_quantity == 0) {
          order_index_.erase(maker->order_id);
          orders.erase(maker);
        }
      }
      if (orders.empty()) {
        level = bids_.erase(level);
      } else {
        break;
      }
    }
  }
}

void OrderBook::add_resting(const NewOrder &command, const Quantity quantity,
                            std::vector<Event> &events) {
  const Price price = *command.limit_price;
  if (command.side == Side::Buy) {
    auto [level, inserted] = bids_.try_emplace(price);
    (void)inserted;
    level->second.orders.push_back(
        RestingOrder{command.order_id, quantity, take_priority_sequence(), command.timestamp});
    auto order = std::prev(level->second.orders.end());
    level->second.aggregate_quantity += quantity;
    order_index_.emplace(command.order_id, IndexEntry{command.side, price, order});
  } else {
    auto [level, inserted] = asks_.try_emplace(price);
    (void)inserted;
    level->second.orders.push_back(
        RestingOrder{command.order_id, quantity, take_priority_sequence(), command.timestamp});
    auto order = std::prev(level->second.orders.end());
    level->second.aggregate_quantity += quantity;
    order_index_.emplace(command.order_id, IndexEntry{command.side, price, order});
  }
  events.emplace_back(Rested{take_event_sequence(), command.order_id, price, quantity});
}

std::vector<Event> OrderBook::process_cancel(const CancelOrder &command) {
  const auto indexed = order_index_.find(command.order_id);
  if (indexed == order_index_.end()) {
    return {Rejected{take_event_sequence(), command.order_id, RejectReason::UnknownOrderId}};
  }
  const Quantity cancelled = indexed->second.order->remaining_quantity;
  erase_indexed(indexed);
  return {Cancelled{take_event_sequence(), command.order_id, cancelled}};
}

std::vector<Event> OrderBook::process_reduce(const ReduceOrder &command) {
  const auto indexed = order_index_.find(command.order_id);
  if (indexed == order_index_.end()) {
    return {Rejected{take_event_sequence(), command.order_id, RejectReason::UnknownOrderId}};
  }
  const Quantity old_quantity = indexed->second.order->remaining_quantity;
  if (command.reduce_by == 0 || command.reduce_by >= old_quantity) {
    return {Rejected{take_event_sequence(), command.order_id, RejectReason::InvalidReduction}};
  }
  const Quantity new_quantity = old_quantity - command.reduce_by;
  if (indexed->second.side == Side::Buy) {
    bids_.at(indexed->second.price).aggregate_quantity -= command.reduce_by;
  } else {
    asks_.at(indexed->second.price).aggregate_quantity -= command.reduce_by;
  }
  indexed->second.order->remaining_quantity = new_quantity;
  return {Reduced{take_event_sequence(), command.order_id, old_quantity, new_quantity}};
}

std::vector<Event> OrderBook::process_replace(const ReplaceOrder &command) {
  const auto indexed = order_index_.find(command.order_id);
  if (indexed == order_index_.end()) {
    return {Rejected{take_event_sequence(), command.order_id, RejectReason::UnknownOrderId}};
  }
  const Price old_price = indexed->second.price;
  const Quantity old_quantity = indexed->second.order->remaining_quantity;
  if (command.new_quantity == 0 || command.new_price <= 0) {
    return {Rejected{take_event_sequence(), command.order_id,
                     command.new_quantity == 0 ? RejectReason::ZeroQuantity
                                               : RejectReason::InvalidPrice}};
  }

  if (command.new_price == old_price && command.new_quantity <= old_quantity) {
    const Quantity reduction = old_quantity - command.new_quantity;
    if (indexed->second.side == Side::Buy) {
      bids_.at(old_price).aggregate_quantity -= reduction;
    } else {
      asks_.at(old_price).aggregate_quantity -= reduction;
    }
    indexed->second.order->remaining_quantity = command.new_quantity;
    return {Replaced{take_event_sequence(), command.order_id, old_price, old_quantity,
                     command.new_price, command.new_quantity, true}};
  }

  const Side side = indexed->second.side;
  const NewOrder replacement{command.order_id,  side,
                             OrderType::Limit,  TimeInForce::GTC,
                             command.new_price, command.new_quantity,
                             command.timestamp};
  const Quantity rest_quantity = predicted_remainder(replacement);
  if (const auto rejection = validate_new(replacement, rest_quantity, command.order_id)) {
    return {Rejected{take_event_sequence(), command.order_id, *rejection}};
  }

  erase_indexed(indexed);
  std::vector<Event> events;
  events.emplace_back(Replaced{take_event_sequence(), command.order_id, old_price, old_quantity,
                               command.new_price, command.new_quantity, false});
  Quantity remaining = command.new_quantity;
  match(replacement, remaining, events);
  if (remaining != 0) {
    add_resting(replacement, remaining, events);
  }
  return events;
}

void OrderBook::erase_indexed(std::unordered_map<OrderId, IndexEntry>::iterator indexed) {
  const Side side = indexed->second.side;
  const Price price = indexed->second.price;
  const Quantity quantity = indexed->second.order->remaining_quantity;
  if (side == Side::Buy) {
    auto level = bids_.find(price);
    level->second.aggregate_quantity -= quantity;
    level->second.orders.erase(indexed->second.order);
    if (level->second.orders.empty()) {
      bids_.erase(level);
    }
  } else {
    auto level = asks_.find(price);
    level->second.aggregate_quantity -= quantity;
    level->second.orders.erase(indexed->second.order);
    if (level->second.orders.empty()) {
      asks_.erase(level);
    }
  }
  order_index_.erase(indexed);
}

std::optional<Price> OrderBook::best_bid() const noexcept {
  return bids_.empty() ? std::nullopt : std::optional<Price>{bids_.begin()->first};
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  return asks_.empty() ? std::nullopt : std::optional<Price>{asks_.begin()->first};
}

Quantity OrderBook::quantity_at(const Side side, const Price price) const noexcept {
  if (side == Side::Buy) {
    const auto level = bids_.find(price);
    return level == bids_.end() ? 0 : level->second.aggregate_quantity;
  }
  const auto level = asks_.find(price);
  return level == asks_.end() ? 0 : level->second.aggregate_quantity;
}

BookSnapshot OrderBook::snapshot(const std::size_t max_levels) const {
  BookSnapshot result;
  const auto append = [max_levels](const auto &levels, auto &destination) {
    for (const auto &[price, level] : levels) {
      if (max_levels != 0 && destination.size() == max_levels) {
        break;
      }
      destination.push_back(DepthLevel{price, level.aggregate_quantity, level.orders.size()});
    }
  };
  append(bids_, result.bids);
  append(asks_, result.asks);
  return result;
}

std::vector<OrderView> OrderBook::active_orders() const {
  std::vector<OrderView> result;
  result.reserve(order_index_.size());
  const auto append = [&result](const auto &levels, const Side side) {
    for (const auto &[price, level] : levels) {
      std::size_t position = 0;
      for (const auto &order : level.orders) {
        result.push_back(OrderView{order.order_id, side, price, order.remaining_quantity,
                                   order.priority_sequence, position++});
      }
    }
  };
  append(bids_, Side::Buy);
  append(asks_, Side::Sell);
  return result;
}

std::optional<OrderView> OrderBook::order(const OrderId order_id) const {
  const auto indexed = order_index_.find(order_id);
  if (indexed == order_index_.end()) {
    return std::nullopt;
  }
  std::size_t position = 0;
  if (indexed->second.side == Side::Buy) {
    for (auto it = bids_.at(indexed->second.price).orders.begin(); it != indexed->second.order;
         ++it) {
      ++position;
    }
  } else {
    for (auto it = asks_.at(indexed->second.price).orders.begin(); it != indexed->second.order;
         ++it) {
      ++position;
    }
  }
  const auto &resting = *indexed->second.order;
  return OrderView{resting.order_id,           indexed->second.side,      indexed->second.price,
                   resting.remaining_quantity, resting.priority_sequence, position};
}

std::size_t OrderBook::active_order_count() const noexcept { return order_index_.size(); }

std::size_t OrderBook::active_price_level_count(const Side side) const noexcept {
  return side == Side::Buy ? bids_.size() : asks_.size();
}

std::string OrderBook::canonical_snapshot() const {
  std::ostringstream stream;
  const auto append = [&stream](const auto &levels, const char side) {
    for (const auto &[price, level] : levels) {
      stream << side << ':' << price << ':' << level.aggregate_quantity;
      for (const auto &order : level.orders) {
        stream << '[' << order.order_id << ',' << order.remaining_quantity << ','
               << order.priority_sequence << ',' << order.timestamp << ']';
      }
      stream << ';';
    }
  };
  append(bids_, 'B');
  append(asks_, 'A');
  return stream.str();
}

bool OrderBook::check_invariants(std::string *error) const {
  const auto fail = [error](const std::string &message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  std::unordered_set<OrderId> reachable;
  std::size_t reachable_count = 0;

  const auto check_levels = [&](const auto &levels, const Side side) {
    for (const auto &[price, level] : levels) {
      if (level.orders.empty()) {
        return fail("empty price level at " + std::to_string(price));
      }
      Quantity aggregate = 0;
      SequenceNumber previous = 0;
      for (const auto &order : level.orders) {
        if (order.remaining_quantity == 0) {
          return fail("zero-quantity active order " + std::to_string(order.order_id));
        }
        if (order.priority_sequence <= previous) {
          return fail("non-increasing FIFO sequence at price " + std::to_string(price));
        }
        previous = order.priority_sequence;
        Quantity next = 0;
        if (!checked_add(aggregate, order.remaining_quantity, next)) {
          return fail("aggregate overflow at price " + std::to_string(price));
        }
        aggregate = next;
        if (!reachable.insert(order.order_id).second) {
          return fail("duplicate reachable order " + std::to_string(order.order_id));
        }
        const auto indexed = order_index_.find(order.order_id);
        if (indexed == order_index_.end()) {
          return fail("reachable order absent from index " + std::to_string(order.order_id));
        }
        if (indexed->second.side != side || indexed->second.price != price ||
            &*indexed->second.order != &order) {
          return fail("index mismatch for order " + std::to_string(order.order_id));
        }
        ++reachable_count;
      }
      if (aggregate != level.aggregate_quantity) {
        return fail("cached aggregate mismatch at price " + std::to_string(price));
      }
    }
    return true;
  };

  if (!check_levels(bids_, Side::Buy) || !check_levels(asks_, Side::Sell)) {
    return false;
  }
  if (reachable_count != order_index_.size()) {
    return fail("index count differs from reachable order count");
  }
  for (const auto &[order_id, indexed] : order_index_) {
    if (!reachable.contains(order_id) || indexed.order->order_id != order_id) {
      return fail("index entry not reachable for order " + std::to_string(order_id));
    }
  }
  if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
    return fail("resting book is crossed");
  }
  return true;
}

} // namespace lob
