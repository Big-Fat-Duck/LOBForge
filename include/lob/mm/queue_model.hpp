#pragma once

#include "lob/mm/types.hpp"

#include <cstdint>
#include <map>

namespace lob::mm {

struct FillEvidence {
  std::uint64_t fillable_quantity{};
  AuditReason reason{AuditReason::QueueNotReached};
};

class QueueTracker final {
public:
  QueueTracker(QueueModel model, ShadowSide side, std::uint32_t limit_price4)
      : model_(model), side_(side), limit_price4_(limit_price4) {}

  void snapshot(const replay::FactualState &state, const std::string &symbol);
  [[nodiscard]] FillEvidence on_mutation(const replay::BookMutation &mutation);
  [[nodiscard]] itch::Shares quantity_ahead() const noexcept { return quantity_ahead_; }
  [[nodiscard]] const std::map<itch::OrderReference, itch::Shares> &orders_ahead() const noexcept {
    return orders_ahead_;
  }
  [[nodiscard]] bool check_invariants() const noexcept;

private:
  QueueModel model_;
  ShadowSide side_;
  std::uint32_t limit_price4_{};
  std::map<itch::OrderReference, itch::Shares> orders_ahead_;
  itch::Shares quantity_ahead_{};

  [[nodiscard]] bool same_side(itch::FeedSide side) const noexcept;
  [[nodiscard]] bool is_trade_through(std::uint32_t factual_price4) const noexcept;
};

} // namespace lob::mm
