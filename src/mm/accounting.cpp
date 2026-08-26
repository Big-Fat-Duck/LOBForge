#include "lob/mm/accounting.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace lob::mm {
namespace {

bool checked_add(const MoneyNanos left, const MoneyNanos right, MoneyNanos &output) noexcept {
  if ((right > 0 && left > std::numeric_limits<MoneyNanos>::max() - right) ||
      (right < 0 && left < std::numeric_limits<MoneyNanos>::min() - right)) {
    return false;
  }
  output = left + right;
  return true;
}

bool checked_multiply(const MoneyNanos value, const std::uint64_t quantity,
                      MoneyNanos &output) noexcept {
  if (value == 0 || quantity == 0) {
    output = 0;
    return true;
  }
  if (value > 0) {
    if (quantity > static_cast<std::uint64_t>(std::numeric_limits<MoneyNanos>::max() / value)) {
      return false;
    }
  } else {
    if (value == std::numeric_limits<MoneyNanos>::min()) {
      return quantity == 1 ? (output = value, true) : false;
    }
    const auto magnitude = static_cast<std::uint64_t>(-value);
    const auto maximum_magnitude =
        static_cast<std::uint64_t>(-(std::numeric_limits<MoneyNanos>::min() + 1)) + 1ULL;
    if (quantity > maximum_magnitude / magnitude) {
      return false;
    }
  }
  output = value * static_cast<MoneyNanos>(quantity);
  return true;
}

std::optional<MoneyNanos> price_nanos(const std::uint32_t price4) noexcept {
  MoneyNanos result = 0;
  if (!checked_multiply(nanos_per_price4, price4, result)) {
    return std::nullopt;
  }
  return result;
}

} // namespace

std::optional<AccountingDelta> AccountingLedger::apply_fill(const ShadowSide side,
                                                            const std::uint64_t quantity,
                                                            const std::uint32_t price4,
                                                            const FeeConfig &fees) {
  if (quantity == 0 || fees.maker_fee_nanos_per_share < 0 ||
      fees.maker_rebate_nanos_per_share < 0 ||
      quantity > std::numeric_limits<std::int64_t>::max()) {
    return std::nullopt;
  }
  const auto unit_price = price_nanos(price4);
  if (!unit_price.has_value()) {
    return std::nullopt;
  }
  MoneyNanos trade_value = 0;
  MoneyNanos fee = 0;
  MoneyNanos rebate = 0;
  if (!checked_multiply(*unit_price, quantity, trade_value) ||
      !checked_multiply(fees.maker_fee_nanos_per_share, quantity, fee) ||
      !checked_multiply(fees.maker_rebate_nanos_per_share, quantity, rebate)) {
    return std::nullopt;
  }

  const auto signed_quantity = static_cast<std::int64_t>(quantity);
  const auto inventory_delta = side == ShadowSide::Buy ? signed_quantity : -signed_quantity;
  MoneyNanos cash_delta = side == ShadowSide::Buy ? -trade_value : trade_value;
  MoneyNanos next_cash = 0;
  MoneyNanos next_fees = 0;
  MoneyNanos next_rebates = 0;
  if ((inventory_delta > 0 &&
       inventory_ > std::numeric_limits<std::int64_t>::max() - inventory_delta) ||
      (inventory_delta < 0 &&
       inventory_ < std::numeric_limits<std::int64_t>::min() - inventory_delta) ||
      !checked_add(trade_cash_nanos_, cash_delta, next_cash) ||
      !checked_add(fees_nanos_, fee, next_fees) ||
      !checked_add(rebates_nanos_, rebate, next_rebates) ||
      turnover_quantity_ > std::numeric_limits<std::uint64_t>::max() - quantity) {
    return std::nullopt;
  }

  auto next_lots = lots_;
  std::uint64_t remainder = quantity;
  MoneyNanos realized_delta = 0;
  while (remainder != 0 && !next_lots.empty() && next_lots.front().side != side) {
    auto &lot = next_lots.front();
    const auto closed = std::min(remainder, lot.quantity);
    const MoneyNanos per_share =
        side == ShadowSide::Sell ? *unit_price - lot.price_nanos : lot.price_nanos - *unit_price;
    MoneyNanos realized_piece = 0;
    MoneyNanos next_realized_delta = 0;
    if (!checked_multiply(per_share, closed, realized_piece) ||
        !checked_add(realized_delta, realized_piece, next_realized_delta)) {
      return std::nullopt;
    }
    realized_delta = next_realized_delta;
    lot.quantity -= closed;
    remainder -= closed;
    if (lot.quantity == 0) {
      next_lots.pop_front();
    }
  }
  if (remainder != 0) {
    next_lots.push_back(Lot{side, remainder, *unit_price});
  }
  MoneyNanos next_realized = 0;
  if (!checked_add(realized_gross_pnl_nanos_, realized_delta, next_realized)) {
    return std::nullopt;
  }

  inventory_ += inventory_delta;
  trade_cash_nanos_ = next_cash;
  fees_nanos_ = next_fees;
  rebates_nanos_ = next_rebates;
  realized_gross_pnl_nanos_ = next_realized;
  turnover_quantity_ += quantity;
  lots_ = std::move(next_lots);
  return AccountingDelta{trade_value, fee, rebate, realized_delta};
}

std::optional<MoneyNanos> AccountingLedger::gross_equity_at_mid2(const std::int64_t mid2) const {
  if (mid2 < 0) {
    return std::nullopt;
  }
  MoneyNanos inventory_value = 0;
  const auto magnitude = inventory_ < 0 ? static_cast<std::uint64_t>(-(inventory_ + 1)) + 1ULL
                                        : static_cast<std::uint64_t>(inventory_);
  MoneyNanos unit = 0;
  if (!checked_multiply(nanos_per_mid2, static_cast<std::uint64_t>(mid2), unit) ||
      !checked_multiply(unit, magnitude, inventory_value)) {
    return std::nullopt;
  }
  if (inventory_ < 0) {
    inventory_value = -inventory_value;
  }
  MoneyNanos result = 0;
  return checked_add(trade_cash_nanos_, inventory_value, result) ? std::optional<MoneyNanos>{result}
                                                                 : std::nullopt;
}

std::optional<MoneyNanos> AccountingLedger::net_equity_at_mid2(const std::int64_t mid2) const {
  const auto gross = gross_equity_at_mid2(mid2);
  MoneyNanos after_fees = 0;
  MoneyNanos result = 0;
  if (!gross.has_value() || !checked_add(*gross, -fees_nanos_, after_fees) ||
      !checked_add(after_fees, rebates_nanos_, result)) {
    return std::nullopt;
  }
  return result;
}

std::optional<MoneyNanos> AccountingLedger::conservative_liquidation_equity(
    const std::uint32_t bid_price4, const std::uint32_t ask_price4,
    const MoneyNanos liquidation_fee_nanos_per_share) const {
  if (liquidation_fee_nanos_per_share < 0) {
    return std::nullopt;
  }
  const auto price = price_nanos(inventory_ >= 0 ? bid_price4 : ask_price4);
  const auto quantity = inventory_ < 0 ? static_cast<std::uint64_t>(-(inventory_ + 1)) + 1ULL
                                       : static_cast<std::uint64_t>(inventory_);
  MoneyNanos value = 0;
  MoneyNanos liquidation_fee = 0;
  if (!price.has_value() || !checked_multiply(*price, quantity, value) ||
      !checked_multiply(liquidation_fee_nanos_per_share, quantity, liquidation_fee)) {
    return std::nullopt;
  }
  if (inventory_ < 0) {
    value = -value;
  }
  MoneyNanos result = 0;
  if (!checked_add(trade_cash_nanos_, value, result) ||
      !checked_add(result, -fees_nanos_, result) || !checked_add(result, rebates_nanos_, result) ||
      !checked_add(result, -liquidation_fee, result)) {
    return std::nullopt;
  }
  return result;
}

bool AccountingLedger::check_invariants() const noexcept {
  std::int64_t lot_inventory = 0;
  for (const auto &lot : lots_) {
    if (lot.quantity == 0 ||
        lot.quantity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    const auto quantity = static_cast<std::int64_t>(lot.quantity);
    const auto signed_quantity = lot.side == ShadowSide::Buy ? quantity : -quantity;
    if ((signed_quantity > 0 &&
         lot_inventory > std::numeric_limits<std::int64_t>::max() - signed_quantity) ||
        (signed_quantity < 0 &&
         lot_inventory < std::numeric_limits<std::int64_t>::min() - signed_quantity)) {
      return false;
    }
    lot_inventory += signed_quantity;
  }
  return lot_inventory == inventory_;
}

} // namespace lob::mm
