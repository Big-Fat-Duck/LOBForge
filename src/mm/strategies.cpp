#include "lob/mm/strategies.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace lob::mm {
namespace {

std::optional<std::uint32_t> observed_bid(const MarketSnapshot &market, const double desired,
                                          const SimulationConfig &config) {
  const auto tick = config.quote.tick_size_price4;
  if (tick == 0 || !std::isfinite(desired) || desired <= 0.0 || market.bids.empty()) {
    return std::nullopt;
  }
  const double rounded = std::floor(desired / static_cast<double>(tick)) * tick;
  const auto best = market.bids.front().price;
  const std::uint64_t maximum_distance =
      static_cast<std::uint64_t>(config.quote.maximum_distance_ticks) * tick;
  for (const auto &level : market.bids) {
    if (static_cast<double>(level.price) <= rounded && best - level.price <= maximum_distance) {
      return level.price;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> observed_ask(const MarketSnapshot &market, const double desired,
                                          const SimulationConfig &config) {
  const auto tick = config.quote.tick_size_price4;
  if (tick == 0 || !std::isfinite(desired) || desired <= 0.0 || market.asks.empty()) {
    return std::nullopt;
  }
  const double rounded = std::ceil(desired / static_cast<double>(tick)) * tick;
  const auto best = market.asks.front().price;
  const std::uint64_t maximum_distance =
      static_cast<std::uint64_t>(config.quote.maximum_distance_ticks) * tick;
  for (const auto &level : market.asks) {
    if (static_cast<double>(level.price) >= rounded && level.price - best <= maximum_distance) {
      return level.price;
    }
  }
  return std::nullopt;
}

} // namespace

QuotePair MarketMakerStrategy::quote(const MarketSnapshot &market, const std::int64_t inventory,
                                     const SimulationConfig &config) const {
  QuotePair result;
  result.quantity = config.quote.quantity;
  if (!market.two_sided) {
    result.bid_reason = AuditReason::MissingBook;
    result.ask_reason = AuditReason::MissingBook;
    return result;
  }
  if (market.locked || market.crossed) {
    result.bid_reason = AuditReason::LockedOrCrossed;
    result.ask_reason = AuditReason::LockedOrCrossed;
    return result;
  }
  if (market.session_phase != replay::SessionPhase::MarketHours || market.trading_state != 'T') {
    result.bid_reason = market.trading_state.has_value() && *market.trading_state != 'T'
                            ? AuditReason::TradingHalt
                            : AuditReason::SessionClosed;
    result.ask_reason = result.bid_reason;
    return result;
  }
  if (!market.mid2.has_value() || config.quote.tick_size_price4 == 0 ||
      config.quote.quantity == 0) {
    result.bid_reason = AuditReason::MissingBook;
    result.ask_reason = AuditReason::MissingBook;
    return result;
  }
  if (market.exchange_timestamp_ns >= config.strategy.session_end_ns) {
    result.bid_reason = AuditReason::CloseCutoff;
    result.ask_reason = AuditReason::CloseCutoff;
    return result;
  }

  const double reference_mid = static_cast<double>(*market.mid2) / 2.0;
  double reservation = reference_mid;
  double total_spread = config.strategy.symmetric_half_spread_price4 * 2.0;
  if (kind_ != StrategyKind::SymmetricQuote) {
    const double gamma = config.strategy.gamma;
    const double sigma_squared = config.strategy.sigma_squared;
    const double k = config.strategy.arrival_intensity_k;
    if (!(gamma > 0.0) || !(k > 0.0) || !(sigma_squared >= 0.0) || !std::isfinite(gamma) ||
        !std::isfinite(k) || !std::isfinite(sigma_squared)) {
      result.bid_reason = AuditReason::IllegalTransition;
      result.ask_reason = AuditReason::IllegalTransition;
      return result;
    }
    const double remaining_seconds =
        static_cast<double>(config.strategy.session_end_ns - market.exchange_timestamp_ns) /
        1'000'000'000.0;
    reservation =
        reference_mid - static_cast<double>(inventory) * gamma * sigma_squared * remaining_seconds;
    if (kind_ == StrategyKind::SignalAwareAs) {
      reservation += config.strategy.signal_coefficient_price4 * market.causal_signal;
    }
    const double risk_term = gamma * sigma_squared * remaining_seconds;
    const double liquidity_term = gamma < 1e-9 ? 2.0 / k : (2.0 / gamma) * std::log1p(gamma / k);
    total_spread = risk_term + liquidity_term;
  }
  if (!std::isfinite(reservation) || !std::isfinite(total_spread) || total_spread < 0.0) {
    result.bid_reason = AuditReason::IllegalTransition;
    result.ask_reason = AuditReason::IllegalTransition;
    return result;
  }
  result.reservation_price4 = reservation;
  result.total_spread_price4 = total_spread;
  const double desired_bid = reservation - total_spread / 2.0;
  const double desired_ask = reservation + total_spread / 2.0;
  result.bid_price4 = observed_bid(market, desired_bid, config);
  result.ask_price4 = observed_ask(market, desired_ask, config);
  result.bid_reason =
      result.bid_price4.has_value() ? AuditReason::None : AuditReason::NoObservedPriceLevel;
  result.ask_reason =
      result.ask_price4.has_value() ? AuditReason::None : AuditReason::NoObservedPriceLevel;

  if (inventory >= config.risk.max_absolute_inventory) {
    result.bid_price4.reset();
    result.bid_reason = AuditReason::InventorySideSuppressed;
  }
  if (inventory <= -config.risk.max_absolute_inventory) {
    result.ask_price4.reset();
    result.ask_reason = AuditReason::InventorySideSuppressed;
  }
  if (result.bid_price4.has_value() && *result.bid_price4 >= market.asks.front().price) {
    result.bid_price4.reset();
    result.bid_reason = AuditReason::CrossesBook;
  }
  if (result.ask_price4.has_value() && *result.ask_price4 <= market.bids.front().price) {
    result.ask_price4.reset();
    result.ask_reason = AuditReason::CrossesBook;
  }
  return result;
}

} // namespace lob::mm
