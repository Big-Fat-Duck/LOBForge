#pragma once

#include "lob/types.hpp"

#include <string_view>
#include <variant>

namespace lob {

enum class RejectReason : std::uint8_t {
  DuplicateOrderId,
  UnknownOrderId,
  ZeroQuantity,
  InvalidPrice,
  InvalidOrderTypeTimeInForce,
  InvalidReduction,
  InvalidReplacement,
  WouldCross,
  CannotFullyFill,
  AggregateOverflow,
  SequenceExhausted
};

[[nodiscard]] constexpr std::string_view to_string(const RejectReason reason) noexcept {
  switch (reason) {
  case RejectReason::DuplicateOrderId:
    return "duplicate_order_id";
  case RejectReason::UnknownOrderId:
    return "unknown_order_id";
  case RejectReason::ZeroQuantity:
    return "zero_quantity";
  case RejectReason::InvalidPrice:
    return "invalid_price";
  case RejectReason::InvalidOrderTypeTimeInForce:
    return "invalid_order_type_time_in_force";
  case RejectReason::InvalidReduction:
    return "invalid_reduction";
  case RejectReason::InvalidReplacement:
    return "invalid_replacement";
  case RejectReason::WouldCross:
    return "would_cross";
  case RejectReason::CannotFullyFill:
    return "cannot_fully_fill";
  case RejectReason::AggregateOverflow:
    return "aggregate_overflow";
  case RejectReason::SequenceExhausted:
    return "sequence_exhausted";
  }
  return "unknown";
}

struct Accepted {
  SequenceNumber sequence{};
  OrderId order_id{};
  Quantity quantity{};
  friend bool operator==(const Accepted &, const Accepted &) = default;
};

struct Rejected {
  SequenceNumber sequence{};
  OrderId order_id{};
  RejectReason reason{RejectReason::InvalidReplacement};
  friend bool operator==(const Rejected &, const Rejected &) = default;
};

struct Trade {
  SequenceNumber sequence{};
  OrderId maker_id{};
  OrderId taker_id{};
  Price resting_price{};
  Quantity quantity{};
  Side aggressive_side{Side::Buy};
  friend bool operator==(const Trade &, const Trade &) = default;
};

struct Rested {
  SequenceNumber sequence{};
  OrderId order_id{};
  Price price{};
  Quantity remaining_quantity{};
  friend bool operator==(const Rested &, const Rested &) = default;
};

struct Cancelled {
  SequenceNumber sequence{};
  OrderId order_id{};
  Quantity cancelled_quantity{};
  friend bool operator==(const Cancelled &, const Cancelled &) = default;
};

struct Reduced {
  SequenceNumber sequence{};
  OrderId order_id{};
  Quantity old_quantity{};
  Quantity new_quantity{};
  friend bool operator==(const Reduced &, const Reduced &) = default;
};

struct Replaced {
  SequenceNumber sequence{};
  OrderId order_id{};
  Price old_price{};
  Quantity old_quantity{};
  Price new_price{};
  Quantity new_quantity{};
  bool priority_retained{};
  friend bool operator==(const Replaced &, const Replaced &) = default;
};

enum class ExpireReason : std::uint8_t { ImmediateOrCancel, MarketRemainder };

struct Expired {
  SequenceNumber sequence{};
  OrderId order_id{};
  Quantity expired_quantity{};
  ExpireReason reason{ExpireReason::ImmediateOrCancel};
  friend bool operator==(const Expired &, const Expired &) = default;
};

using Event =
    std::variant<Accepted, Rejected, Trade, Rested, Cancelled, Reduced, Replaced, Expired>;

[[nodiscard]] SequenceNumber event_sequence(const Event &event) noexcept;

} // namespace lob
