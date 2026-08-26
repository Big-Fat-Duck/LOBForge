#pragma once

#include "lob/replay/factual_book.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lob::mm {

using ShadowOrderId = std::uint64_t;
using LocalSequence = std::uint64_t;
using TimestampNs = std::uint64_t;
using MoneyNanos = std::int64_t;

constexpr MoneyNanos nanos_per_price4 = 100'000;
constexpr MoneyNanos nanos_per_mid2 = 50'000;

enum class ShadowSide : std::uint8_t { Buy, Sell };
enum class QueueModel : std::uint8_t { MboFifoConservative, TradeThroughOnly, FrontOfQueue };
enum class StrategyKind : std::uint8_t { SymmetricQuote, AvellanedaStoikov, SignalAwareAs };
enum class ShadowOrderState : std::uint8_t {
  Submitted,
  PendingAck,
  Active,
  PendingCancel,
  PendingReplace,
  PartiallyFilled,
  Filled,
  Rejected,
  Cancelled,
  SessionEnded,
  RiskCancelled
};
enum class AuditReason : std::uint8_t {
  None,
  Accepted,
  Activated,
  CrossesBook,
  QuantityLimit,
  InventoryLimit,
  OpenOrderLimit,
  NotionalLimit,
  StaleMarket,
  TradingHalt,
  SessionClosed,
  CloseCutoff,
  StopSwitch,
  MaximumLoss,
  MaximumDrawdown,
  CancelRequested,
  CancelConfirmed,
  ReplaceRequested,
  ReplaceConfirmed,
  FactualExecution,
  TradeThrough,
  QueueNotReached,
  SessionEnd,
  RiskForcedCancel,
  IllegalTransition,
  ArithmeticOverflow,
  MissingBook,
  LockedOrCrossed,
  NoObservedPriceLevel,
  MinimumRestTime,
  RefreshNotRequired,
  InventorySideSuppressed,
  SequenceGap
};

[[nodiscard]] std::string_view to_string(ShadowSide value) noexcept;
[[nodiscard]] std::string_view to_string(QueueModel value) noexcept;
[[nodiscard]] std::string_view to_string(StrategyKind value) noexcept;
[[nodiscard]] std::string_view to_string(ShadowOrderState value) noexcept;
[[nodiscard]] std::string_view to_string(AuditReason value) noexcept;
[[nodiscard]] std::optional<QueueModel> parse_queue_model(std::string_view value) noexcept;
[[nodiscard]] std::optional<StrategyKind> parse_strategy_kind(std::string_view value) noexcept;

struct LatencyConfig {
  TimestampNs market_data_ns{};
  TimestampNs strategy_compute_ns{};
  TimestampNs order_entry_ns{};
  TimestampNs cancel_ns{};
  TimestampNs replace_ns{};
  friend bool operator==(const LatencyConfig &, const LatencyConfig &) = default;
};

struct FeeConfig {
  MoneyNanos maker_fee_nanos_per_share{};
  MoneyNanos maker_rebate_nanos_per_share{};
  MoneyNanos liquidation_fee_nanos_per_share{};
  friend bool operator==(const FeeConfig &, const FeeConfig &) = default;
};

struct RiskLimits {
  std::int64_t max_absolute_inventory{1'000};
  std::uint64_t max_order_quantity{100};
  std::uint64_t max_open_orders{2};
  MoneyNanos max_notional_nanos{10'000'000'000'000LL};
  TimestampNs max_market_staleness_ns{1'000'000'000ULL};
  MoneyNanos maximum_loss_nanos{1'000'000'000'000LL};
  MoneyNanos maximum_drawdown_nanos{1'000'000'000'000LL};
  TimestampNs stop_new_quotes_before_close_ns{60'000'000'000ULL};
  friend bool operator==(const RiskLimits &, const RiskLimits &) = default;
};

struct QuoteConfig {
  std::uint64_t quantity{10};
  TimestampNs minimum_rest_ns{10'000'000ULL};
  std::uint32_t refresh_threshold_ticks{1};
  std::uint32_t maximum_distance_ticks{10};
  std::uint32_t tick_size_price4{100};
  friend bool operator==(const QuoteConfig &, const QuoteConfig &) = default;
};

struct StrategyParameters {
  double gamma{0.1};
  double sigma_squared{1.0};
  double arrival_intensity_k{1.0};
  double symmetric_half_spread_price4{100.0};
  double signal_coefficient_price4{};
  TimestampNs session_end_ns{57'600'000'000'000ULL};
  std::string fitted_partition{"train"};
  std::string fitted_protocol_sha256{"unknown"};
};

struct SimulationConfig {
  LatencyConfig latency;
  FeeConfig fees;
  RiskLimits risk;
  QuoteConfig quote;
  StrategyParameters strategy;
  QueueModel queue_model{QueueModel::MboFifoConservative};
  StrategyKind strategy_kind{StrategyKind::AvellanedaStoikov};
  bool automatic_strategy{true};
  std::uint64_t random_seed{20'260'826ULL};
  std::vector<TimestampNs> markout_horizons_ns{10'000'000ULL, 100'000'000ULL, 1'000'000'000ULL};
  std::string protocol_sha256{"unknown"};
};

struct MarketSnapshot {
  TimestampNs exchange_timestamp_ns{};
  std::uint64_t factual_sequence{};
  std::string symbol;
  replay::SessionPhase session_phase{replay::SessionPhase::BeforeMessages};
  std::optional<char> trading_state;
  std::vector<replay::FactualDepthLevel> bids;
  std::vector<replay::FactualDepthLevel> asks;
  std::optional<std::int64_t> mid2;
  std::optional<double> imbalance_l1;
  double causal_signal{};
  bool two_sided{};
  bool locked{};
  bool crossed{};
};

struct QuotePair {
  std::optional<std::uint32_t> bid_price4;
  std::optional<std::uint32_t> ask_price4;
  std::uint64_t quantity{};
  std::optional<double> reservation_price4;
  std::optional<double> total_spread_price4;
  AuditReason bid_reason{AuditReason::None};
  AuditReason ask_reason{AuditReason::None};
};

struct ShadowOrder {
  ShadowOrderId order_id{};
  std::string symbol;
  ShadowSide side{ShadowSide::Buy};
  std::uint32_t limit_price4{};
  std::uint64_t original_quantity{};
  std::uint64_t remaining_quantity{};
  ShadowOrderState state{ShadowOrderState::Submitted};
  TimestampNs submit_timestamp_ns{};
  std::optional<TimestampNs> active_timestamp_ns;
  std::optional<TimestampNs> terminal_timestamp_ns;
  LocalSequence priority_sequence{};
  std::map<itch::OrderReference, itch::Shares> queue_ahead;
  itch::Shares queue_ahead_quantity{};
};

struct ShadowOrderEvent {
  std::string schema{"lobforge.shadow_order"};
  std::uint32_t version{1};
  TimestampNs timestamp_ns{};
  LocalSequence local_sequence{};
  ShadowOrderId order_id{};
  std::string symbol;
  ShadowSide side{ShadowSide::Buy};
  std::uint32_t price4{};
  std::uint64_t quantity{};
  std::uint64_t remaining_quantity{};
  ShadowOrderState from_state{ShadowOrderState::Submitted};
  ShadowOrderState to_state{ShadowOrderState::Submitted};
  AuditReason reason{AuditReason::None};
  itch::Shares queue_ahead_quantity{};
};

struct ShadowFill {
  std::string schema{"lobforge.shadow_fill"};
  std::uint32_t version{1};
  TimestampNs timestamp_ns{};
  std::uint64_t factual_sequence{};
  LocalSequence local_sequence{};
  ShadowOrderId order_id{};
  std::string symbol;
  ShadowSide side{ShadowSide::Buy};
  std::uint64_t quantity{};
  std::uint32_t accounting_price4{};
  std::uint32_t factual_display_price4{};
  std::optional<std::uint32_t> factual_execution_price4;
  std::optional<std::int64_t> anchor_mid2;
  std::optional<itch::MatchNumber> match_number;
  AuditReason reason{AuditReason::FactualExecution};
  MoneyNanos fee_nanos{};
  MoneyNanos rebate_nanos{};
};

struct InventoryEvent {
  std::string schema{"lobforge.inventory_event"};
  std::uint32_t version{1};
  TimestampNs timestamp_ns{};
  LocalSequence local_sequence{};
  ShadowOrderId order_id{};
  std::int64_t inventory{};
  MoneyNanos trade_cash_nanos{};
  MoneyNanos fees_nanos{};
  MoneyNanos rebates_nanos{};
  MoneyNanos realized_gross_pnl_nanos{};
  std::optional<MoneyNanos> gross_equity_nanos;
  std::optional<MoneyNanos> net_equity_nanos;
  std::optional<MoneyNanos> conservative_liquidation_equity_nanos;
};

struct MarkoutResult {
  TimestampNs horizon_ns{};
  std::uint64_t eligible_fills{};
  std::uint64_t eligible_quantity{};
  std::uint64_t missing_fills{};
  MoneyNanos directional_value_nanos{};
  MoneyNanos adverse_selection_cost_nanos{};
};

struct SimulationSummary {
  std::string schema{"lobforge.mm_summary"};
  std::uint32_t version{1};
  std::string protocol_sha256{"unknown"};
  std::uint64_t factual_events{};
  std::uint64_t order_event_rows{};
  std::uint64_t inventory_event_rows{};
  std::uint64_t submitted_orders{};
  std::uint64_t submitted_quantity{};
  std::uint64_t acknowledged_orders{};
  std::uint64_t rejected_orders{};
  std::uint64_t cancelled_orders{};
  std::uint64_t replaced_orders{};
  std::uint64_t filled_orders{};
  std::uint64_t fill_events{};
  std::uint64_t filled_quantity{};
  std::uint64_t turnover_quantity{};
  std::uint64_t risk_suppressions{};
  std::uint64_t stop_switch_triggers{};
  std::int64_t final_inventory{};
  std::int64_t maximum_absolute_inventory{};
  std::uint64_t inventory_limit{};
  MoneyNanos trade_cash_nanos{};
  MoneyNanos fees_nanos{};
  MoneyNanos rebates_nanos{};
  std::optional<MoneyNanos> gross_pnl_nanos;
  std::optional<MoneyNanos> net_pnl_nanos;
  std::optional<MoneyNanos> conservative_liquidation_pnl_nanos;
  MoneyNanos realized_gross_pnl_nanos{};
  std::optional<MoneyNanos> unrealized_gross_pnl_nanos;
  MoneyNanos spread_capture_nanos{};
  MoneyNanos maximum_drawdown_nanos{};
  std::uint64_t quoted_time_ns{};
  std::uint64_t eligible_market_time_ns{};
  long double time_weighted_absolute_inventory{};
  long double rms_inventory{};
  std::vector<MarkoutResult> markouts;
  std::string semantic_digest;
};

} // namespace lob::mm
