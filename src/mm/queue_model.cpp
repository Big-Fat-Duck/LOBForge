#include "lob/mm/queue_model.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace lob::mm {

bool QueueTracker::same_side(const itch::FeedSide side) const noexcept {
  return (side_ == ShadowSide::Buy && side == itch::FeedSide::Buy) ||
         (side_ == ShadowSide::Sell && side == itch::FeedSide::Sell);
}

bool QueueTracker::is_trade_through(const std::uint32_t factual_price4) const noexcept {
  return side_ == ShadowSide::Buy ? factual_price4 < limit_price4_ : factual_price4 > limit_price4_;
}

void QueueTracker::snapshot(const replay::FactualState &state, const std::string &symbol) {
  orders_ahead_.clear();
  quantity_ahead_ = 0;
  if (model_ != QueueModel::MboFifoConservative) {
    return;
  }
  for (const auto &order : state.active_orders()) {
    if (order.symbol != symbol || !same_side(order.side) || order.display_price != limit_price4_) {
      continue;
    }
    orders_ahead_.emplace(order.order_reference, order.remaining_shares);
    quantity_ahead_ += order.remaining_shares;
  }
}

FillEvidence QueueTracker::on_mutation(const replay::BookMutation &mutation) {
  if (!same_side(mutation.side)) {
    return {};
  }
  const bool execution = mutation.message_type == 'E' || mutation.message_type == 'C';
  if (execution && is_trade_through(mutation.display_price)) {
    return {mutation.event_quantity, AuditReason::TradeThrough};
  }
  if (mutation.display_price != limit_price4_ && mutation.message_type != 'U') {
    return {};
  }
  if (model_ == QueueModel::TradeThroughOnly) {
    return {};
  }
  if (model_ == QueueModel::FrontOfQueue) {
    return execution && mutation.display_price == limit_price4_
               ? FillEvidence{mutation.event_quantity, AuditReason::FactualExecution}
               : FillEvidence{};
  }

  auto ahead = orders_ahead_.find(mutation.order_reference);
  if (ahead != orders_ahead_.end()) {
    if (mutation.message_type == 'U' || mutation.message_type == 'D') {
      quantity_ahead_ -= ahead->second;
      orders_ahead_.erase(ahead);
      return {};
    }
    if (mutation.message_type == 'E' || mutation.message_type == 'C' ||
        mutation.message_type == 'X') {
      const auto removed = std::min<itch::Shares>(ahead->second, mutation.event_quantity);
      ahead->second -= removed;
      quantity_ahead_ -= removed;
      if (ahead->second == 0) {
        orders_ahead_.erase(ahead);
      }
      return {};
    }
  }
  if (execution && mutation.display_price == limit_price4_ && quantity_ahead_ == 0) {
    return {mutation.event_quantity, AuditReason::FactualExecution};
  }
  return {};
}

bool QueueTracker::check_invariants() const noexcept {
  itch::Shares sum = 0;
  for (const auto &[reference, quantity] : orders_ahead_) {
    (void)reference;
    if (quantity == 0 || sum > std::numeric_limits<itch::Shares>::max() - quantity) {
      return false;
    }
    sum += quantity;
  }
  return sum == quantity_ahead_;
}

} // namespace lob::mm
