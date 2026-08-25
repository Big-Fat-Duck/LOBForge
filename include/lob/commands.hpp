#pragma once

#include "lob/types.hpp"

#include <optional>
#include <variant>

namespace lob {

struct NewOrder {
  OrderId order_id{};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TimeInForce time_in_force{TimeInForce::GTC};
  std::optional<Price> limit_price{};
  Quantity quantity{};
  Timestamp timestamp{};

  friend bool operator==(const NewOrder &, const NewOrder &) = default;
};

struct CancelOrder {
  OrderId order_id{};
  friend bool operator==(const CancelOrder &, const CancelOrder &) = default;
};

struct ReduceOrder {
  OrderId order_id{};
  Quantity reduce_by{};
  friend bool operator==(const ReduceOrder &, const ReduceOrder &) = default;
};

struct ReplaceOrder {
  OrderId order_id{};
  Price new_price{};
  Quantity new_quantity{};
  Timestamp timestamp{};
  friend bool operator==(const ReplaceOrder &, const ReplaceOrder &) = default;
};

using Command = std::variant<NewOrder, CancelOrder, ReduceOrder, ReplaceOrder>;

} // namespace lob
