#pragma once

#include "lob/mm/types.hpp"

#include <cstdint>
#include <deque>
#include <optional>

namespace lob::mm {

struct AccountingDelta {
  MoneyNanos trade_value_nanos{};
  MoneyNanos fee_nanos{};
  MoneyNanos rebate_nanos{};
  MoneyNanos realized_gross_pnl_nanos{};
};

class AccountingLedger final {
public:
  [[nodiscard]] std::optional<AccountingDelta>
  apply_fill(ShadowSide side, std::uint64_t quantity, std::uint32_t price4, const FeeConfig &fees);

  [[nodiscard]] std::int64_t inventory() const noexcept { return inventory_; }
  [[nodiscard]] MoneyNanos trade_cash_nanos() const noexcept { return trade_cash_nanos_; }
  [[nodiscard]] MoneyNanos fees_nanos() const noexcept { return fees_nanos_; }
  [[nodiscard]] MoneyNanos rebates_nanos() const noexcept { return rebates_nanos_; }
  [[nodiscard]] MoneyNanos realized_gross_pnl_nanos() const noexcept {
    return realized_gross_pnl_nanos_;
  }
  [[nodiscard]] std::uint64_t turnover_quantity() const noexcept { return turnover_quantity_; }
  [[nodiscard]] std::optional<MoneyNanos> gross_equity_at_mid2(std::int64_t mid2) const;
  [[nodiscard]] std::optional<MoneyNanos> net_equity_at_mid2(std::int64_t mid2) const;
  [[nodiscard]] std::optional<MoneyNanos>
  conservative_liquidation_equity(std::uint32_t bid_price4, std::uint32_t ask_price4,
                                  MoneyNanos liquidation_fee_nanos_per_share) const;
  [[nodiscard]] bool check_invariants() const noexcept;

private:
  struct Lot {
    ShadowSide side{ShadowSide::Buy};
    std::uint64_t quantity{};
    MoneyNanos price_nanos{};
  };

  std::int64_t inventory_{};
  MoneyNanos trade_cash_nanos_{};
  MoneyNanos fees_nanos_{};
  MoneyNanos rebates_nanos_{};
  MoneyNanos realized_gross_pnl_nanos_{};
  std::uint64_t turnover_quantity_{};
  std::deque<Lot> lots_;
};

} // namespace lob::mm
