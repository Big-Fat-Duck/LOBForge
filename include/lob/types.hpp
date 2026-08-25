#pragma once

#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using SequenceNumber = std::uint64_t;
using Timestamp = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK, PostOnly };

[[nodiscard]] constexpr Side opposite(const Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace lob
