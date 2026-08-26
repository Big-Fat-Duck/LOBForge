#include "lob/mm/simulator.hpp"

#include "lob/itch/messages.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace lob::mm {
namespace {

TimestampNs add_time(const TimestampNs left, const TimestampNs right) noexcept {
  return left > std::numeric_limits<TimestampNs>::max() - right
             ? std::numeric_limits<TimestampNs>::max()
             : left + right;
}

std::string digest_hex(const std::string &value) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << hash;
  return output.str();
}

std::int64_t absolute_inventory(const std::int64_t value) noexcept {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return value < 0 ? -value : value;
}

bool checked_multiply(const MoneyNanos left, const std::uint64_t right,
                      MoneyNanos &result) noexcept {
  if (right == 0 || left == 0) {
    result = 0;
    return true;
  }
  if (right > static_cast<std::uint64_t>(std::numeric_limits<MoneyNanos>::max())) {
    return false;
  }
  const auto signed_right = static_cast<MoneyNanos>(right);
  if ((left > 0 && left > std::numeric_limits<MoneyNanos>::max() / signed_right) ||
      (left < 0 && left < std::numeric_limits<MoneyNanos>::min() / signed_right)) {
    return false;
  }
  result = left * signed_right;
  return true;
}

bool checked_add(const MoneyNanos left, const MoneyNanos right, MoneyNanos &result) noexcept {
  if ((right > 0 && left > std::numeric_limits<MoneyNanos>::max() - right) ||
      (right < 0 && left < std::numeric_limits<MoneyNanos>::min() - right)) {
    return false;
  }
  result = left + right;
  return result != std::numeric_limits<MoneyNanos>::min();
}

bool terminal_state(const ShadowOrderState state) noexcept {
  return state == ShadowOrderState::Filled || state == ShadowOrderState::Rejected ||
         state == ShadowOrderState::Cancelled || state == ShadowOrderState::SessionEnded ||
         state == ShadowOrderState::RiskCancelled;
}

bool valid_transition(const ShadowOrderState from, const ShadowOrderState to) noexcept {
  switch (from) {
  case ShadowOrderState::Submitted:
    return to == ShadowOrderState::PendingAck;
  case ShadowOrderState::PendingAck:
    return to == ShadowOrderState::Active || to == ShadowOrderState::Rejected ||
           to == ShadowOrderState::SessionEnded || to == ShadowOrderState::RiskCancelled;
  case ShadowOrderState::Active:
  case ShadowOrderState::PartiallyFilled:
    return to == ShadowOrderState::PendingCancel || to == ShadowOrderState::PendingReplace ||
           to == ShadowOrderState::PartiallyFilled || to == ShadowOrderState::Filled ||
           to == ShadowOrderState::SessionEnded || to == ShadowOrderState::RiskCancelled;
  case ShadowOrderState::PendingCancel:
    return to == ShadowOrderState::Cancelled || to == ShadowOrderState::Filled ||
           to == ShadowOrderState::SessionEnded || to == ShadowOrderState::RiskCancelled;
  case ShadowOrderState::PendingReplace:
    return to == ShadowOrderState::Active || to == ShadowOrderState::Filled ||
           to == ShadowOrderState::SessionEnded || to == ShadowOrderState::RiskCancelled;
  case ShadowOrderState::Filled:
  case ShadowOrderState::Rejected:
  case ShadowOrderState::Cancelled:
  case ShadowOrderState::SessionEnded:
  case ShadowOrderState::RiskCancelled:
    return false;
  }
  return false;
}

} // namespace

ShadowSimulator::ShadowSimulator(SimulationConfig config)
    : config_(std::move(config)), strategy_(config_.strategy_kind) {}

std::uint8_t ShadowSimulator::category(const ScheduledKind kind) noexcept {
  switch (kind) {
  case ScheduledKind::Observation:
    return 0;
  case ScheduledKind::Decision:
    return 1;
  case ScheduledKind::Activate:
    return 2;
  case ScheduledKind::CancelConfirm:
    return 3;
  case ScheduledKind::ReplaceConfirm:
    return 4;
  }
  return 255;
}

bool ShadowSimulator::ScheduledLess::operator()(const ScheduledEvent &left,
                                                const ScheduledEvent &right) const noexcept {
  return std::tuple{left.timestamp_ns, left.factual_sequence, ShadowSimulator::category(left.kind),
                    left.local_sequence} < std::tuple{right.timestamp_ns, right.factual_sequence,
                                                      ShadowSimulator::category(right.kind),
                                                      right.local_sequence};
}

void ShadowSimulator::schedule(ScheduledEvent event) {
  event.local_sequence = next_local_sequence_++;
  scheduled_.insert(std::move(event));
}

void ShadowSimulator::accumulate_time(const TimestampNs timestamp_ns) {
  if (!last_accounting_timestamp_.has_value()) {
    last_accounting_timestamp_ = timestamp_ns;
    return;
  }
  if (timestamp_ns <= *last_accounting_timestamp_) {
    return;
  }
  const auto duration = timestamp_ns - *last_accounting_timestamp_;
  const long double inventory = static_cast<long double>(ledger_.inventory());
  absolute_inventory_time_integral_ += std::fabs(inventory) * duration;
  squared_inventory_time_integral_ += inventory * inventory * duration;
  if (last_market_eligible_) {
    eligible_market_time_ns_ += duration;
  }
  if (last_market_quoted_) {
    quoted_time_ns_ += duration;
  }
  last_accounting_timestamp_ = timestamp_ns;
}

void ShadowSimulator::before_market(const TimestampNs timestamp_ns,
                                    const std::uint64_t factual_sequence,
                                    const replay::FactualState &state) {
  if (last_factual_timestamp_.has_value() && timestamp_ns > *last_factual_timestamp_) {
    drain_through(*last_factual_timestamp_, state);
  }
  drain_before(timestamp_ns, state);
  if (last_factual_sequence_.has_value() && factual_sequence != *last_factual_sequence_ + 1) {
    trigger_stop(AuditReason::SequenceGap, timestamp_ns);
  }
}

void ShadowSimulator::after_market(const itch::Message &message,
                                   const std::uint64_t factual_sequence,
                                   const replay::FactualState &state) {
  const auto timestamp_ns = itch::common_header(message).timestamp;
  accumulate_time(timestamp_ns);
  ++factual_events_;
  if (state.last_book_mutation().has_value()) {
    apply_market_mutation(*state.last_book_mutation(), timestamp_ns, factual_sequence);
  }
  update_market(message, factual_sequence, state);
  update_risk(timestamp_ns);
  if (state.session_phase() == replay::SessionPhase::MarketHoursEnded ||
      state.session_phase() == replay::SessionPhase::SystemHoursEnded ||
      state.session_phase() == replay::SessionPhase::MessagesEnded) {
    force_cancel_all(AuditReason::SessionEnd, timestamp_ns);
  }
  last_factual_timestamp_ = timestamp_ns;
  last_factual_sequence_ = factual_sequence;
}

ShadowOrderId ShadowSimulator::manual_submit(std::string symbol, const ShadowSide side,
                                             const std::uint32_t price4,
                                             const std::uint64_t quantity,
                                             const TimestampNs decision_timestamp_ns) {
  accumulate_time(decision_timestamp_ns);
  const auto order_id = next_order_id_++;
  ShadowOrder order{order_id,
                    std::move(symbol),
                    side,
                    price4,
                    quantity,
                    quantity,
                    ShadowOrderState::Submitted,
                    decision_timestamp_ns,
                    std::nullopt,
                    std::nullopt,
                    0,
                    {},
                    0};
  auto [position, inserted] = orders_.emplace(order_id, std::move(order));
  (void)inserted;
  ++submitted_orders_;
  if (submitted_quantity_ <= std::numeric_limits<std::uint64_t>::max() - quantity) {
    submitted_quantity_ += quantity;
  } else {
    trigger_stop(AuditReason::ArithmeticOverflow, decision_timestamp_ns);
  }
  record_order_event(position->second, ShadowOrderState::Submitted, ShadowOrderState::Submitted,
                     AuditReason::Accepted, decision_timestamp_ns);
  (void)transition(position->second, ShadowOrderState::PendingAck, AuditReason::Accepted,
                   decision_timestamp_ns);
  ScheduledEvent activation;
  activation.timestamp_ns = add_time(decision_timestamp_ns, config_.latency.order_entry_ns);
  activation.factual_sequence = last_factual_sequence_.value_or(0);
  activation.kind = ScheduledKind::Activate;
  activation.order_id = order_id;
  schedule(std::move(activation));
  return order_id;
}

bool ShadowSimulator::manual_cancel(const ShadowOrderId order_id,
                                    const TimestampNs decision_timestamp_ns) {
  auto position = orders_.find(order_id);
  if (position == orders_.end() || (position->second.state != ShadowOrderState::Active &&
                                    position->second.state != ShadowOrderState::PartiallyFilled)) {
    if (position != orders_.end()) {
      record_order_event(position->second, position->second.state, position->second.state,
                         AuditReason::IllegalTransition, decision_timestamp_ns);
    }
    return false;
  }
  if (!transition(position->second, ShadowOrderState::PendingCancel, AuditReason::CancelRequested,
                  decision_timestamp_ns)) {
    return false;
  }
  ScheduledEvent confirmation;
  confirmation.timestamp_ns = add_time(decision_timestamp_ns, config_.latency.cancel_ns);
  confirmation.factual_sequence = last_factual_sequence_.value_or(0);
  confirmation.kind = ScheduledKind::CancelConfirm;
  confirmation.order_id = order_id;
  schedule(std::move(confirmation));
  return true;
}

bool ShadowSimulator::manual_replace(const ShadowOrderId order_id, const std::uint32_t new_price4,
                                     const std::uint64_t new_quantity,
                                     const TimestampNs decision_timestamp_ns) {
  auto position = orders_.find(order_id);
  if (position == orders_.end() || (position->second.state != ShadowOrderState::Active &&
                                    position->second.state != ShadowOrderState::PartiallyFilled)) {
    if (position != orders_.end()) {
      record_order_event(position->second, position->second.state, position->second.state,
                         AuditReason::IllegalTransition, decision_timestamp_ns);
    }
    return false;
  }
  if (!transition(position->second, ShadowOrderState::PendingReplace, AuditReason::ReplaceRequested,
                  decision_timestamp_ns)) {
    return false;
  }
  ScheduledEvent confirmation;
  confirmation.timestamp_ns = add_time(decision_timestamp_ns, config_.latency.replace_ns);
  confirmation.factual_sequence = last_factual_sequence_.value_or(0);
  confirmation.kind = ScheduledKind::ReplaceConfirm;
  confirmation.order_id = order_id;
  confirmation.price4 = new_price4;
  confirmation.quantity = new_quantity;
  schedule(std::move(confirmation));
  return true;
}

void ShadowSimulator::drain_before(const TimestampNs timestamp_ns,
                                   const replay::FactualState &state) {
  while (!scheduled_.empty() && scheduled_.begin()->timestamp_ns < timestamp_ns) {
    const auto event = *scheduled_.begin();
    scheduled_.erase(scheduled_.begin());
    execute_scheduled(event, state);
  }
}

void ShadowSimulator::drain_through(const TimestampNs timestamp_ns,
                                    const replay::FactualState &state) {
  while (!scheduled_.empty() && scheduled_.begin()->timestamp_ns <= timestamp_ns) {
    const auto event = *scheduled_.begin();
    scheduled_.erase(scheduled_.begin());
    execute_scheduled(event, state);
  }
}

void ShadowSimulator::execute_scheduled(const ScheduledEvent &event,
                                        const replay::FactualState &state) {
  accumulate_time(event.timestamp_ns);
  switch (event.kind) {
  case ScheduledKind::Observation:
    execute_observation(event);
    break;
  case ScheduledKind::Decision:
    execute_decision(event);
    break;
  case ScheduledKind::Activate:
    activate(event.order_id, event.timestamp_ns, state);
    break;
  case ScheduledKind::CancelConfirm:
    confirm_cancel(event.order_id, event.timestamp_ns);
    break;
  case ScheduledKind::ReplaceConfirm:
    confirm_replace(event, state);
    break;
  }
}

void ShadowSimulator::execute_observation(const ScheduledEvent &event) {
  if (!config_.automatic_strategy) {
    return;
  }
  ScheduledEvent decision;
  decision.timestamp_ns = add_time(event.timestamp_ns, config_.latency.strategy_compute_ns);
  decision.factual_sequence = event.factual_sequence;
  decision.kind = ScheduledKind::Decision;
  decision.market = event.market;
  schedule(std::move(decision));
}

void ShadowSimulator::execute_decision(const ScheduledEvent &event) {
  if (!config_.automatic_strategy || stop_switch_) {
    return;
  }
  const auto quotes = strategy_.quote(event.market, ledger_.inventory(), config_);
  manage_quote(event.market, ShadowSide::Buy, quotes.bid_price4, quotes.quantity, quotes.bid_reason,
               event.timestamp_ns);
  manage_quote(event.market, ShadowSide::Sell, quotes.ask_price4, quotes.quantity,
               quotes.ask_reason, event.timestamp_ns);
}

void ShadowSimulator::manage_quote(const MarketSnapshot &market, const ShadowSide side,
                                   const std::optional<std::uint32_t> desired_price,
                                   const std::uint64_t quantity,
                                   const AuditReason suppression_reason,
                                   const TimestampNs decision_timestamp_ns) {
  const auto existing = open_order(market.symbol, side);
  if (!desired_price.has_value()) {
    if (suppression_reason != AuditReason::None) {
      ++risk_suppressions_;
    }
    if (existing.has_value()) {
      (void)manual_cancel(*existing, decision_timestamp_ns);
    }
    return;
  }
  if (!existing.has_value()) {
    (void)manual_submit(market.symbol, side, *desired_price, quantity, decision_timestamp_ns);
    return;
  }
  auto &order = orders_.at(*existing);
  if (order.state != ShadowOrderState::Active && order.state != ShadowOrderState::PartiallyFilled) {
    return;
  }
  const auto difference = order.limit_price4 > *desired_price ? order.limit_price4 - *desired_price
                                                              : *desired_price - order.limit_price4;
  const auto threshold = config_.quote.refresh_threshold_ticks * config_.quote.tick_size_price4;
  if (difference < threshold) {
    return;
  }
  if (!order.active_timestamp_ns.has_value() ||
      decision_timestamp_ns - *order.active_timestamp_ns < config_.quote.minimum_rest_ns) {
    record_order_event(order, order.state, order.state, AuditReason::MinimumRestTime,
                       decision_timestamp_ns);
    return;
  }
  (void)manual_replace(order.order_id, *desired_price, quantity, decision_timestamp_ns);
}

std::optional<ShadowOrderId> ShadowSimulator::open_order(const std::string &symbol,
                                                         const ShadowSide side) const {
  for (const auto &[order_id, order] : orders_) {
    if (order.symbol == symbol && order.side == side && is_open_state(order.state)) {
      return order_id;
    }
  }
  return std::nullopt;
}

AuditReason ShadowSimulator::activation_risk(const ShadowOrder &order,
                                             const TimestampNs timestamp_ns,
                                             const replay::FactualState &state) const {
  if (stop_switch_) {
    return AuditReason::StopSwitch;
  }
  if (order.original_quantity == 0 || order.original_quantity > config_.risk.max_order_quantity) {
    return AuditReason::QuantityLimit;
  }
  if (order.remaining_quantity >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return AuditReason::QuantityLimit;
  }
  std::uint64_t open = 0;
  for (const auto &[order_id, candidate] : orders_) {
    (void)order_id;
    if (candidate.order_id != order.order_id && is_open_state(candidate.state)) {
      ++open;
    }
  }
  if (open >= config_.risk.max_open_orders) {
    return AuditReason::OpenOrderLimit;
  }
  const auto signed_remaining = static_cast<std::int64_t>(order.remaining_quantity);
  if ((order.side == ShadowSide::Buy &&
       ledger_.inventory() > config_.risk.max_absolute_inventory - signed_remaining) ||
      (order.side == ShadowSide::Sell &&
       ledger_.inventory() < -config_.risk.max_absolute_inventory + signed_remaining)) {
    return AuditReason::InventoryLimit;
  }
  const auto worst_inventory = order.side == ShadowSide::Buy
                                   ? ledger_.inventory() + signed_remaining
                                   : ledger_.inventory() - signed_remaining;
  if (state.session_phase() != replay::SessionPhase::MarketHours) {
    return AuditReason::SessionClosed;
  }
  const auto market = latest_markets_.find(order.symbol);
  if (market == latest_markets_.end()) {
    return AuditReason::MissingBook;
  }
  if (timestamp_ns < market->second.exchange_timestamp_ns ||
      timestamp_ns - market->second.exchange_timestamp_ns > config_.risk.max_market_staleness_ns) {
    return AuditReason::StaleMarket;
  }
  if (market->second.session_phase != replay::SessionPhase::MarketHours) {
    return AuditReason::SessionClosed;
  }
  if (market->second.trading_state != 'T') {
    return AuditReason::TradingHalt;
  }
  if (timestamp_ns >= config_.strategy.session_end_ns ||
      config_.strategy.session_end_ns - timestamp_ns <=
          config_.risk.stop_new_quotes_before_close_ns) {
    return AuditReason::CloseCutoff;
  }
  if (!market->second.two_sided) {
    return AuditReason::MissingBook;
  }
  if (market->second.locked || market->second.crossed) {
    return AuditReason::LockedOrCrossed;
  }
  if ((order.side == ShadowSide::Buy && order.limit_price4 >= market->second.asks.front().price) ||
      (order.side == ShadowSide::Sell && order.limit_price4 <= market->second.bids.front().price)) {
    return AuditReason::CrossesBook;
  }
  const std::uint64_t unit =
      static_cast<std::uint64_t>(order.limit_price4) * static_cast<std::uint64_t>(nanos_per_price4);
  const auto worst_magnitude = absolute_inventory(worst_inventory);
  if (worst_magnitude != 0 && unit > static_cast<std::uint64_t>(config_.risk.max_notional_nanos) /
                                         static_cast<std::uint64_t>(worst_magnitude)) {
    return AuditReason::NotionalLimit;
  }
  return AuditReason::None;
}

void ShadowSimulator::activate(const ShadowOrderId order_id, const TimestampNs timestamp_ns,
                               const replay::FactualState &state) {
  auto position = orders_.find(order_id);
  if (position == orders_.end() || position->second.state != ShadowOrderState::PendingAck) {
    return;
  }
  auto &order = position->second;
  const auto risk = activation_risk(order, timestamp_ns, state);
  if (risk != AuditReason::None) {
    ++rejected_orders_;
    ++risk_suppressions_;
    (void)transition(order, ShadowOrderState::Rejected, risk, timestamp_ns);
    return;
  }
  order.active_timestamp_ns = timestamp_ns;
  order.priority_sequence = next_priority_sequence_++;
  QueueTracker tracker(config_.queue_model, order.side, order.limit_price4);
  tracker.snapshot(state, order.symbol);
  order.queue_ahead = tracker.orders_ahead();
  order.queue_ahead_quantity = tracker.quantity_ahead();
  queue_trackers_.insert_or_assign(order_id, std::move(tracker));
  ++acknowledged_orders_;
  (void)transition(order, ShadowOrderState::Active, AuditReason::Activated, timestamp_ns);
}

void ShadowSimulator::confirm_cancel(const ShadowOrderId order_id, const TimestampNs timestamp_ns) {
  auto position = orders_.find(order_id);
  if (position == orders_.end() || position->second.state != ShadowOrderState::PendingCancel) {
    return;
  }
  ++cancelled_orders_;
  queue_trackers_.erase(order_id);
  (void)transition(position->second, ShadowOrderState::Cancelled, AuditReason::CancelConfirmed,
                   timestamp_ns);
}

void ShadowSimulator::confirm_replace(const ScheduledEvent &event,
                                      const replay::FactualState &state) {
  auto position = orders_.find(event.order_id);
  if (position == orders_.end() || position->second.state != ShadowOrderState::PendingReplace) {
    return;
  }
  auto &order = position->second;
  order.limit_price4 = event.price4;
  order.original_quantity = event.quantity;
  order.remaining_quantity = event.quantity;
  const auto risk = activation_risk(order, event.timestamp_ns, state);
  if (risk != AuditReason::None) {
    ++rejected_orders_;
    ++risk_suppressions_;
    queue_trackers_.erase(order.order_id);
    (void)transition(order, ShadowOrderState::RiskCancelled, risk, event.timestamp_ns);
    return;
  }
  order.active_timestamp_ns = event.timestamp_ns;
  order.priority_sequence = next_priority_sequence_++;
  QueueTracker tracker(config_.queue_model, order.side, order.limit_price4);
  tracker.snapshot(state, order.symbol);
  order.queue_ahead = tracker.orders_ahead();
  order.queue_ahead_quantity = tracker.quantity_ahead();
  queue_trackers_.insert_or_assign(order.order_id, std::move(tracker));
  ++replaced_orders_;
  (void)transition(order, ShadowOrderState::Active, AuditReason::ReplaceConfirmed,
                   event.timestamp_ns);
}

void ShadowSimulator::apply_market_mutation(const replay::BookMutation &mutation,
                                            const TimestampNs timestamp_ns,
                                            const std::uint64_t factual_sequence) {
  std::vector<ShadowOrderId> candidates;
  for (const auto &[order_id, order] : orders_) {
    if (order.symbol == mutation.symbol && is_open_state(order.state) &&
        order.state != ShadowOrderState::PendingAck) {
      candidates.push_back(order_id);
    }
  }
  for (const auto order_id : candidates) {
    auto order_position = orders_.find(order_id);
    auto tracker_position = queue_trackers_.find(order_id);
    if (order_position == orders_.end() || tracker_position == queue_trackers_.end() ||
        terminal_state(order_position->second.state)) {
      continue;
    }
    const auto evidence = tracker_position->second.on_mutation(mutation);
    order_position->second.queue_ahead = tracker_position->second.orders_ahead();
    order_position->second.queue_ahead_quantity = tracker_position->second.quantity_ahead();
    if (evidence.fillable_quantity != 0) {
      apply_fill(order_position->second,
                 std::min(evidence.fillable_quantity, order_position->second.remaining_quantity),
                 mutation, timestamp_ns, factual_sequence, evidence.reason);
    }
  }
}

void ShadowSimulator::apply_fill(ShadowOrder &order, const std::uint64_t quantity,
                                 const replay::BookMutation &mutation,
                                 const TimestampNs timestamp_ns,
                                 const std::uint64_t factual_sequence, const AuditReason reason) {
  if (quantity == 0 || quantity > order.remaining_quantity) {
    return;
  }
  const auto accounting =
      ledger_.apply_fill(order.side, quantity, order.limit_price4, config_.fees);
  if (!accounting.has_value()) {
    trigger_stop(AuditReason::ArithmeticOverflow, timestamp_ns);
    return;
  }
  order.remaining_quantity -= quantity;
  ShadowFill fill;
  fill.timestamp_ns = timestamp_ns;
  fill.factual_sequence = factual_sequence;
  fill.local_sequence = next_local_sequence_++;
  fill.order_id = order.order_id;
  fill.symbol = order.symbol;
  fill.side = order.side;
  fill.quantity = quantity;
  fill.accounting_price4 = order.limit_price4;
  fill.factual_display_price4 = mutation.display_price;
  fill.factual_execution_price4 = mutation.execution_price;
  const auto market = latest_markets_.find(order.symbol);
  if (market != latest_markets_.end()) {
    fill.anchor_mid2 = market->second.mid2;
  }
  fill.match_number = mutation.match_number;
  fill.reason = reason;
  fill.fee_nanos = accounting->fee_nanos;
  fill.rebate_nanos = accounting->rebate_nanos;
  fills_.push_back(fill);
  InventoryEvent inventory;
  inventory.timestamp_ns = timestamp_ns;
  inventory.local_sequence = next_local_sequence_++;
  inventory.order_id = order.order_id;
  inventory.inventory = ledger_.inventory();
  inventory.trade_cash_nanos = ledger_.trade_cash_nanos();
  inventory.fees_nanos = ledger_.fees_nanos();
  inventory.rebates_nanos = ledger_.rebates_nanos();
  inventory.realized_gross_pnl_nanos = ledger_.realized_gross_pnl_nanos();
  if (market != latest_markets_.end() && market->second.mid2.has_value()) {
    inventory.gross_equity_nanos = ledger_.gross_equity_at_mid2(*market->second.mid2);
    inventory.net_equity_nanos = ledger_.net_equity_at_mid2(*market->second.mid2);
    if (!market->second.bids.empty() && !market->second.asks.empty()) {
      inventory.conservative_liquidation_equity_nanos = ledger_.conservative_liquidation_equity(
          market->second.bids.front().price, market->second.asks.front().price,
          config_.fees.liquidation_fee_nanos_per_share);
    }
  }
  inventory_events_.push_back(inventory);
  maximum_absolute_inventory_ =
      std::max(maximum_absolute_inventory_, absolute_inventory(ledger_.inventory()));
  if (order.remaining_quantity == 0) {
    ++filled_orders_;
    queue_trackers_.erase(order.order_id);
    (void)transition(order, ShadowOrderState::Filled, reason, timestamp_ns);
  } else if (order.state == ShadowOrderState::Active ||
             order.state == ShadowOrderState::PartiallyFilled) {
    (void)transition(order, ShadowOrderState::PartiallyFilled, reason, timestamp_ns);
  } else {
    record_order_event(order, order.state, order.state, reason, timestamp_ns);
  }
}

void ShadowSimulator::record_order_event(ShadowOrder &order, const ShadowOrderState from,
                                         const ShadowOrderState to, const AuditReason reason,
                                         const TimestampNs timestamp_ns) {
  order_events_.push_back(ShadowOrderEvent{
      "lobforge.shadow_order", 1, timestamp_ns, next_local_sequence_++, order.order_id,
      order.symbol, order.side, order.limit_price4, order.original_quantity,
      order.remaining_quantity, from, to, reason, order.queue_ahead_quantity});
  last_market_quoted_ = std::any_of(orders_.begin(), orders_.end(), [](const auto &entry) {
    const auto state = entry.second.state;
    return state == ShadowOrderState::Active || state == ShadowOrderState::PartiallyFilled ||
           state == ShadowOrderState::PendingCancel || state == ShadowOrderState::PendingReplace;
  });
}

bool ShadowSimulator::transition(ShadowOrder &order, const ShadowOrderState to,
                                 const AuditReason reason, const TimestampNs timestamp_ns) {
  const auto from = order.state;
  if (from == to) {
    record_order_event(order, from, to, reason, timestamp_ns);
    return true;
  }
  if (!valid_transition(from, to)) {
    record_order_event(order, from, from, AuditReason::IllegalTransition, timestamp_ns);
    return false;
  }
  order.state = to;
  if (terminal_state(to)) {
    order.terminal_timestamp_ns = timestamp_ns;
  }
  record_order_event(order, from, to, reason, timestamp_ns);
  return true;
}

MarketSnapshot ShadowSimulator::snapshot_for(const std::string &symbol,
                                             const TimestampNs timestamp_ns,
                                             const std::uint64_t factual_sequence,
                                             const replay::FactualState &state) const {
  MarketSnapshot result;
  result.exchange_timestamp_ns = timestamp_ns;
  result.factual_sequence = factual_sequence;
  result.symbol = symbol;
  result.session_phase = state.session_phase();
  result.trading_state = state.trading_state(symbol);
  result.bids = state.depth(symbol, itch::FeedSide::Buy, 10);
  result.asks = state.depth(symbol, itch::FeedSide::Sell, 10);
  result.two_sided = !result.bids.empty() && !result.asks.empty();
  if (result.two_sided) {
    result.locked = result.bids.front().price == result.asks.front().price;
    result.crossed = result.bids.front().price > result.asks.front().price;
    if (!result.crossed) {
      result.mid2 = static_cast<std::int64_t>(result.bids.front().price) +
                    static_cast<std::int64_t>(result.asks.front().price);
      const long double bid = result.bids.front().shares;
      const long double ask = result.asks.front().shares;
      if (bid + ask != 0.0L) {
        result.imbalance_l1 = static_cast<double>((bid - ask) / (bid + ask));
        result.causal_signal = *result.imbalance_l1;
      }
    }
  }
  return result;
}

void ShadowSimulator::update_market(const itch::Message &message,
                                    const std::uint64_t factual_sequence,
                                    const replay::FactualState &state) {
  const auto &header = itch::common_header(message);
  auto symbol = state.symbol_for_locate(header.stock_locate);
  if (!symbol.has_value() && state.last_book_mutation().has_value()) {
    symbol = state.last_book_mutation()->symbol;
  }
  if (!symbol.has_value()) {
    return;
  }
  auto snapshot = snapshot_for(*symbol, header.timestamp, factual_sequence, state);
  latest_markets_.insert_or_assign(*symbol, snapshot);
  last_market_eligible_ = snapshot.two_sided && !snapshot.crossed && !snapshot.locked &&
                          snapshot.session_phase == replay::SessionPhase::MarketHours &&
                          snapshot.trading_state == 'T';
  last_market_quoted_ = open_order(*symbol, ShadowSide::Buy).has_value() ||
                        open_order(*symbol, ShadowSide::Sell).has_value();
  const bool valid =
      snapshot.mid2.has_value() && snapshot.two_sided && !snapshot.crossed && !snapshot.locked &&
      snapshot.session_phase == replay::SessionPhase::MarketHours && snapshot.trading_state == 'T';
  market_samples_[*symbol].push_back(
      MarketSample{header.timestamp, factual_sequence, valid, snapshot.mid2.value_or(0),
                   snapshot.bids.empty() ? 0U : snapshot.bids.front().price,
                   snapshot.asks.empty() ? 0U : snapshot.asks.front().price});
  ScheduledEvent observation;
  observation.timestamp_ns = add_time(header.timestamp, config_.latency.market_data_ns);
  observation.factual_sequence = factual_sequence;
  observation.kind = ScheduledKind::Observation;
  observation.market = std::move(snapshot);
  schedule(std::move(observation));
}

void ShadowSimulator::update_risk(const TimestampNs timestamp_ns) {
  for (const auto &[symbol, market] : latest_markets_) {
    (void)symbol;
    if (!market.mid2.has_value()) {
      continue;
    }
    const auto equity = ledger_.net_equity_at_mid2(*market.mid2);
    if (!equity.has_value()) {
      trigger_stop(AuditReason::ArithmeticOverflow, timestamp_ns);
      return;
    }
    peak_net_equity_nanos_ = std::max(peak_net_equity_nanos_, *equity);
    const auto drawdown = peak_net_equity_nanos_ - *equity;
    maximum_drawdown_nanos_ = std::max(maximum_drawdown_nanos_, drawdown);
    if (*equity <= -config_.risk.maximum_loss_nanos) {
      trigger_stop(AuditReason::MaximumLoss, timestamp_ns);
      return;
    }
    if (drawdown >= config_.risk.maximum_drawdown_nanos) {
      trigger_stop(AuditReason::MaximumDrawdown, timestamp_ns);
      return;
    }
  }
}

void ShadowSimulator::trigger_stop(const AuditReason reason, const TimestampNs timestamp_ns) {
  if (stop_switch_) {
    return;
  }
  stop_switch_ = true;
  ++stop_switch_triggers_;
  force_cancel_all(reason, timestamp_ns);
}

void ShadowSimulator::force_cancel_all(const AuditReason reason, const TimestampNs timestamp_ns) {
  for (auto &[order_id, order] : orders_) {
    (void)order_id;
    if (!is_open_state(order.state)) {
      continue;
    }
    queue_trackers_.erase(order.order_id);
    const auto terminal = reason == AuditReason::SessionEnd ? ShadowOrderState::SessionEnded
                                                            : ShadowOrderState::RiskCancelled;
    (void)transition(order, terminal, reason, timestamp_ns);
    ++cancelled_orders_;
  }
}

bool ShadowSimulator::is_open_state(const ShadowOrderState state) const noexcept {
  return !terminal_state(state) && state != ShadowOrderState::Submitted;
}

void ShadowSimulator::finish(const TimestampNs timestamp_ns, const replay::FactualState &state) {
  if (last_factual_timestamp_.has_value()) {
    drain_through(timestamp_ns, state);
  }
  accumulate_time(timestamp_ns);
  force_cancel_all(AuditReason::SessionEnd, timestamp_ns);
  scheduled_.clear();
}

bool ShadowSimulator::check_invariants(std::string *error) const {
  const auto fail = [&](const std::string &message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (!ledger_.check_invariants()) {
    return fail("accounting lot inventory mismatch");
  }
  std::map<std::pair<std::string, ShadowSide>, std::uint64_t> open_per_side;
  for (const auto &[order_id, order] : orders_) {
    if (order.order_id != order_id || order.remaining_quantity > order.original_quantity) {
      return fail("shadow order identity or remaining quantity invalid");
    }
    if (is_open_state(order.state)) {
      if (++open_per_side[{order.symbol, order.side}] > 1) {
        return fail("more than one open quote per symbol side");
      }
    }
    const auto tracker = queue_trackers_.find(order_id);
    if (tracker != queue_trackers_.end() &&
        (!tracker->second.check_invariants() ||
         tracker->second.quantity_ahead() != order.queue_ahead_quantity)) {
      return fail("queue tracker invariant failure");
    }
  }
  return true;
}

std::string ShadowSimulator::canonical_state() const {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "protocol=" << config_.protocol_sha256 << '\n';
  output << "queue=" << to_string(config_.queue_model) << '\n';
  output << "strategy=" << to_string(config_.strategy_kind) << '\n';
  output << "inventory=" << ledger_.inventory() << '\n';
  output << "cash=" << ledger_.trade_cash_nanos() << '\n';
  output << "fees=" << ledger_.fees_nanos() << '\n';
  output << "rebates=" << ledger_.rebates_nanos() << '\n';
  output << "stop=" << stop_switch_ << '\n';
  for (const auto &[order_id, order] : orders_) {
    output << "order=" << order_id << '|' << order.symbol << '|' << to_string(order.side) << '|'
           << order.limit_price4 << '|' << order.original_quantity << '|'
           << order.remaining_quantity << '|' << to_string(order.state) << '|'
           << order.priority_sequence << '|' << order.queue_ahead_quantity << '\n';
    for (const auto &[reference, quantity] : order.queue_ahead) {
      output << "ahead=" << reference << '|' << quantity << '\n';
    }
  }
  for (const auto &fill : fills_) {
    output << "fill=" << fill.timestamp_ns << '|' << fill.factual_sequence << '|'
           << fill.local_sequence << '|' << fill.order_id << '|' << fill.quantity << '|'
           << fill.accounting_price4 << '|' << to_string(fill.reason) << '\n';
  }
  return output.str();
}

std::string ShadowSimulator::semantic_digest() const { return digest_hex(canonical_state()); }

std::vector<MarkoutResult> ShadowSimulator::calculate_markouts() const {
  std::vector<MarkoutResult> output;
  for (const auto horizon : config_.markout_horizons_ns) {
    MarkoutResult result;
    result.horizon_ns = horizon;
    for (const auto &fill : fills_) {
      const auto samples = market_samples_.find(fill.symbol);
      const auto due = add_time(fill.timestamp_ns, horizon);
      if (samples == market_samples_.end() || samples->second.empty() ||
          samples->second.back().timestamp_ns < due) {
        ++result.missing_fills;
        continue;
      }
      const auto position =
          std::upper_bound(samples->second.begin(), samples->second.end(),
                           std::pair{due, std::numeric_limits<std::uint64_t>::max()},
                           [](const auto &key, const MarketSample &sample) {
                             return key < std::pair{sample.timestamp_ns, sample.factual_sequence};
                           });
      if (position == samples->second.begin()) {
        ++result.missing_fills;
        continue;
      }
      const auto &sample = *std::prev(position);
      if (!sample.valid) {
        ++result.missing_fills;
        continue;
      }
      const auto delta_mid2 = sample.mid2 - static_cast<std::int64_t>(fill.accounting_price4) * 2;
      const auto directional_delta = fill.side == ShadowSide::Buy ? delta_mid2 : -delta_mid2;
      MoneyNanos value_per_share = 0;
      MoneyNanos value = 0;
      MoneyNanos accumulated = 0;
      if (!checked_multiply(directional_delta, static_cast<std::uint64_t>(nanos_per_mid2),
                            value_per_share) ||
          !checked_multiply(value_per_share, fill.quantity, value) ||
          !checked_add(result.directional_value_nanos, value, accumulated) ||
          result.eligible_quantity > std::numeric_limits<std::uint64_t>::max() - fill.quantity) {
        ++result.missing_fills;
        continue;
      }
      result.directional_value_nanos = accumulated;
      result.eligible_quantity += fill.quantity;
      ++result.eligible_fills;
    }
    result.adverse_selection_cost_nanos = -result.directional_value_nanos;
    output.push_back(result);
  }
  return output;
}

SimulationSummary ShadowSimulator::summary() const {
  SimulationSummary result;
  result.protocol_sha256 = config_.protocol_sha256;
  result.factual_events = factual_events_;
  result.order_event_rows = static_cast<std::uint64_t>(order_events_.size());
  result.inventory_event_rows = static_cast<std::uint64_t>(inventory_events_.size());
  result.submitted_orders = submitted_orders_;
  result.submitted_quantity = submitted_quantity_;
  result.acknowledged_orders = acknowledged_orders_;
  result.rejected_orders = rejected_orders_;
  result.cancelled_orders = cancelled_orders_;
  result.replaced_orders = replaced_orders_;
  result.filled_orders = filled_orders_;
  result.fill_events = fills_.size();
  for (const auto &fill : fills_) {
    result.filled_quantity += fill.quantity;
    if (fill.anchor_mid2.has_value()) {
      const auto delta = *fill.anchor_mid2 - static_cast<std::int64_t>(fill.accounting_price4) * 2;
      const auto direction = fill.side == ShadowSide::Buy ? 1LL : -1LL;
      const long double capture = static_cast<long double>(direction) * delta *
                                  static_cast<long double>(nanos_per_mid2) * fill.quantity;
      if (capture <= std::numeric_limits<MoneyNanos>::max() &&
          capture >= std::numeric_limits<MoneyNanos>::min()) {
        const auto value = static_cast<MoneyNanos>(capture);
        if ((value >= 0 &&
             result.spread_capture_nanos <= std::numeric_limits<MoneyNanos>::max() - value) ||
            (value < 0 &&
             result.spread_capture_nanos >= std::numeric_limits<MoneyNanos>::min() - value)) {
          result.spread_capture_nanos += value;
        }
      }
    }
  }
  result.turnover_quantity = ledger_.turnover_quantity();
  result.risk_suppressions = risk_suppressions_;
  result.stop_switch_triggers = stop_switch_triggers_;
  result.final_inventory = ledger_.inventory();
  result.maximum_absolute_inventory = maximum_absolute_inventory_;
  result.inventory_limit = static_cast<std::uint64_t>(config_.risk.max_absolute_inventory);
  result.trade_cash_nanos = ledger_.trade_cash_nanos();
  result.fees_nanos = ledger_.fees_nanos();
  result.rebates_nanos = ledger_.rebates_nanos();
  result.realized_gross_pnl_nanos = ledger_.realized_gross_pnl_nanos();
  if (!market_samples_.empty() && !market_samples_.begin()->second.empty()) {
    const auto &terminal = market_samples_.begin()->second.back();
    if (terminal.valid) {
      const auto gross = ledger_.gross_equity_at_mid2(terminal.mid2);
      const auto net = ledger_.net_equity_at_mid2(terminal.mid2);
      const auto liquidation = ledger_.conservative_liquidation_equity(
          terminal.bid_price4, terminal.ask_price4, config_.fees.liquidation_fee_nanos_per_share);
      result.gross_pnl_nanos = gross;
      result.net_pnl_nanos = net;
      result.conservative_liquidation_pnl_nanos = liquidation;
      if (gross.has_value()) {
        result.unrealized_gross_pnl_nanos = *gross - result.realized_gross_pnl_nanos;
      }
    }
  }
  result.maximum_drawdown_nanos = maximum_drawdown_nanos_;
  result.quoted_time_ns = quoted_time_ns_;
  result.eligible_market_time_ns = eligible_market_time_ns_;
  if (eligible_market_time_ns_ != 0) {
    result.time_weighted_absolute_inventory =
        absolute_inventory_time_integral_ / eligible_market_time_ns_;
    result.rms_inventory = std::sqrt(squared_inventory_time_integral_ / eligible_market_time_ns_);
  }
  result.markouts = calculate_markouts();
  result.semantic_digest = semantic_digest();
  return result;
}

} // namespace lob::mm
