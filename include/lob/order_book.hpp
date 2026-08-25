#pragma once

#include "lob/commands.hpp"
#include "lob/events.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

struct DepthLevel {
  Price price{};
  Quantity quantity{};
  std::size_t order_count{};
  friend bool operator==(const DepthLevel &, const DepthLevel &) = default;
};

struct BookSnapshot {
  std::vector<DepthLevel> bids;
  std::vector<DepthLevel> asks;
  friend bool operator==(const BookSnapshot &, const BookSnapshot &) = default;
};

struct OrderView {
  OrderId order_id{};
  Side side{Side::Buy};
  Price price{};
  Quantity remaining_quantity{};
  SequenceNumber priority_sequence{};
  std::size_t queue_position{};
  friend bool operator==(const OrderView &, const OrderView &) = default;
};

class OrderBook final {
public:
  OrderBook() = default;
  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;
  OrderBook(OrderBook &&) = default;
  OrderBook &operator=(OrderBook &&) = default;
  ~OrderBook() = default;

  [[nodiscard]] std::vector<Event> process(const Command &command);

  [[nodiscard]] std::optional<Price> best_bid() const noexcept;
  [[nodiscard]] std::optional<Price> best_ask() const noexcept;
  [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept;
  [[nodiscard]] BookSnapshot snapshot(std::size_t max_levels = 0) const;
  [[nodiscard]] std::vector<OrderView> active_orders() const;
  [[nodiscard]] std::optional<OrderView> order(OrderId order_id) const;
  [[nodiscard]] std::size_t active_order_count() const noexcept;
  [[nodiscard]] std::size_t active_price_level_count(Side side) const noexcept;
  [[nodiscard]] std::string canonical_snapshot() const;

  // Intended for assertions in debug builds and for every step of randomized tests.
  [[nodiscard]] bool check_invariants(std::string *error = nullptr) const;

private:
  struct RestingOrder {
    OrderId order_id{};
    Quantity remaining_quantity{};
    SequenceNumber priority_sequence{};
    Timestamp timestamp{};
  };

  struct PriceLevel {
    Quantity aggregate_quantity{};
    std::list<RestingOrder> orders;
  };

  using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskMap = std::map<Price, PriceLevel, std::less<Price>>;
  using OrderIterator = std::list<RestingOrder>::iterator;

  struct IndexEntry {
    Side side{Side::Buy};
    Price price{};
    OrderIterator order;
  };

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, IndexEntry> order_index_;
  SequenceNumber next_event_sequence_{1};
  SequenceNumber next_priority_sequence_{1};

  [[nodiscard]] std::vector<Event> process_new(const NewOrder &command);
  [[nodiscard]] std::vector<Event> process_cancel(const CancelOrder &command);
  [[nodiscard]] std::vector<Event> process_reduce(const ReduceOrder &command);
  [[nodiscard]] std::vector<Event> process_replace(const ReplaceOrder &command);

  [[nodiscard]] SequenceNumber take_event_sequence();
  [[nodiscard]] SequenceNumber take_priority_sequence();
  [[nodiscard]] std::optional<RejectReason> validate_new(const NewOrder &command,
                                                         Quantity rest_quantity,
                                                         std::optional<OrderId> replacing) const;
  [[nodiscard]] Quantity executable_quantity(const NewOrder &command) const noexcept;
  [[nodiscard]] bool would_cross(Side side, Price price) const noexcept;
  [[nodiscard]] Quantity predicted_remainder(const NewOrder &command) const noexcept;
  [[nodiscard]] bool can_add_to_level(Side side, Price price, Quantity quantity,
                                      std::optional<OrderId> replacing) const noexcept;

  void match(NewOrder command, Quantity &remaining, std::vector<Event> &events);
  void add_resting(const NewOrder &command, Quantity quantity, std::vector<Event> &events);
  void erase_indexed(std::unordered_map<OrderId, IndexEntry>::iterator indexed);
};

} // namespace lob
