#include "lob/mm/types.hpp"

namespace lob::mm {

std::string_view to_string(const ShadowSide value) noexcept {
  return value == ShadowSide::Buy ? "buy" : "sell";
}

std::string_view to_string(const QueueModel value) noexcept {
  switch (value) {
  case QueueModel::MboFifoConservative:
    return "mbo_fifo_conservative";
  case QueueModel::TradeThroughOnly:
    return "trade_through_only";
  case QueueModel::FrontOfQueue:
    return "front_of_queue";
  }
  return "unknown";
}

std::string_view to_string(const StrategyKind value) noexcept {
  switch (value) {
  case StrategyKind::SymmetricQuote:
    return "symmetric_quote";
  case StrategyKind::AvellanedaStoikov:
    return "avellaneda_stoikov";
  case StrategyKind::SignalAwareAs:
    return "signal_aware_as";
  }
  return "unknown";
}

std::string_view to_string(const ShadowOrderState value) noexcept {
  switch (value) {
  case ShadowOrderState::Submitted:
    return "submitted";
  case ShadowOrderState::PendingAck:
    return "pending_ack";
  case ShadowOrderState::Active:
    return "active";
  case ShadowOrderState::PendingCancel:
    return "pending_cancel";
  case ShadowOrderState::PendingReplace:
    return "pending_replace";
  case ShadowOrderState::PartiallyFilled:
    return "partially_filled";
  case ShadowOrderState::Filled:
    return "filled";
  case ShadowOrderState::Rejected:
    return "rejected";
  case ShadowOrderState::Cancelled:
    return "cancelled";
  case ShadowOrderState::SessionEnded:
    return "session_ended";
  case ShadowOrderState::RiskCancelled:
    return "risk_cancelled";
  }
  return "unknown";
}

std::string_view to_string(const AuditReason value) noexcept {
  switch (value) {
  case AuditReason::None:
    return "none";
  case AuditReason::Accepted:
    return "accepted";
  case AuditReason::Activated:
    return "activated";
  case AuditReason::CrossesBook:
    return "crosses_book";
  case AuditReason::QuantityLimit:
    return "quantity_limit";
  case AuditReason::InventoryLimit:
    return "inventory_limit";
  case AuditReason::OpenOrderLimit:
    return "open_order_limit";
  case AuditReason::NotionalLimit:
    return "notional_limit";
  case AuditReason::StaleMarket:
    return "stale_market";
  case AuditReason::TradingHalt:
    return "trading_halt";
  case AuditReason::SessionClosed:
    return "session_closed";
  case AuditReason::CloseCutoff:
    return "close_cutoff";
  case AuditReason::StopSwitch:
    return "stop_switch";
  case AuditReason::MaximumLoss:
    return "maximum_loss";
  case AuditReason::MaximumDrawdown:
    return "maximum_drawdown";
  case AuditReason::CancelRequested:
    return "cancel_requested";
  case AuditReason::CancelConfirmed:
    return "cancel_confirmed";
  case AuditReason::ReplaceRequested:
    return "replace_requested";
  case AuditReason::ReplaceConfirmed:
    return "replace_confirmed";
  case AuditReason::FactualExecution:
    return "factual_execution";
  case AuditReason::TradeThrough:
    return "trade_through";
  case AuditReason::QueueNotReached:
    return "queue_not_reached";
  case AuditReason::SessionEnd:
    return "session_end";
  case AuditReason::RiskForcedCancel:
    return "risk_forced_cancel";
  case AuditReason::IllegalTransition:
    return "illegal_transition";
  case AuditReason::ArithmeticOverflow:
    return "arithmetic_overflow";
  case AuditReason::MissingBook:
    return "missing_book";
  case AuditReason::LockedOrCrossed:
    return "locked_or_crossed";
  case AuditReason::NoObservedPriceLevel:
    return "no_observed_price_level";
  case AuditReason::MinimumRestTime:
    return "minimum_rest_time";
  case AuditReason::RefreshNotRequired:
    return "refresh_not_required";
  case AuditReason::InventorySideSuppressed:
    return "inventory_side_suppressed";
  case AuditReason::SequenceGap:
    return "sequence_gap";
  }
  return "unknown";
}

std::optional<QueueModel> parse_queue_model(const std::string_view value) noexcept {
  if (value == "mbo_fifo_conservative") {
    return QueueModel::MboFifoConservative;
  }
  if (value == "trade_through_only") {
    return QueueModel::TradeThroughOnly;
  }
  if (value == "front_of_queue") {
    return QueueModel::FrontOfQueue;
  }
  return std::nullopt;
}

std::optional<StrategyKind> parse_strategy_kind(const std::string_view value) noexcept {
  if (value == "symmetric_quote") {
    return StrategyKind::SymmetricQuote;
  }
  if (value == "avellaneda_stoikov") {
    return StrategyKind::AvellanedaStoikov;
  }
  if (value == "signal_aware_as") {
    return StrategyKind::SignalAwareAs;
  }
  return std::nullopt;
}

} // namespace lob::mm
