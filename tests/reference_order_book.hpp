#pragma once

#include "lob/order_book.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace lob::test {

// Deliberately scan-based oracle: simple ordering rules are preferred over production complexity.
class ReferenceOrderBook final {
public:
  [[nodiscard]] std::vector<Event> process(const Command &command);
  [[nodiscard]] std::optional<Price> best_bid() const;
  [[nodiscard]] std::optional<Price> best_ask() const;
  [[nodiscard]] BookSnapshot snapshot() const;
  [[nodiscard]] std::vector<OrderView> active_orders() const;

private:
  struct Order {
    OrderId order_id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    SequenceNumber priority_sequence{};
    Timestamp timestamp{};
  };

  std::vector<Order> orders_;
  SequenceNumber next_event_sequence_{1};
  SequenceNumber next_priority_sequence_{1};

  [[nodiscard]] SequenceNumber event_sequence() { return next_event_sequence_++; }
  [[nodiscard]] SequenceNumber priority_sequence() { return next_priority_sequence_++; }
  [[nodiscard]] auto find(OrderId order_id) {
    return std::find_if(orders_.begin(), orders_.end(),
                        [order_id](const Order &order) { return order.order_id == order_id; });
  }
  [[nodiscard]] auto find(OrderId order_id) const {
    return std::find_if(orders_.begin(), orders_.end(),
                        [order_id](const Order &order) { return order.order_id == order_id; });
  }
  [[nodiscard]] std::optional<RejectReason>
  validate(const NewOrder &command, Quantity rest_quantity, std::optional<OrderId> replacing) const;
  [[nodiscard]] Quantity executable(const NewOrder &command) const;
  [[nodiscard]] Quantity aggregate(Side side, Price price,
                                   std::optional<OrderId> excluding = std::nullopt) const;
  void match(const NewOrder &command, Quantity &remaining, std::vector<Event> &events);
  void rest(const NewOrder &command, Quantity remaining, std::vector<Event> &events);
  [[nodiscard]] std::vector<Event> process_new(const NewOrder &command);
  [[nodiscard]] std::vector<Event> process_cancel(const CancelOrder &command);
  [[nodiscard]] std::vector<Event> process_reduce(const ReduceOrder &command);
  [[nodiscard]] std::vector<Event> process_replace(const ReplaceOrder &command);
};

} // namespace lob::test
