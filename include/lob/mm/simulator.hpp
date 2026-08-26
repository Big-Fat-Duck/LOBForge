#pragma once

#include "lob/mm/accounting.hpp"
#include "lob/mm/queue_model.hpp"
#include "lob/mm/strategies.hpp"
#include "lob/mm/types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lob::mm {

class ShadowSimulator final {
public:
  explicit ShadowSimulator(SimulationConfig config);

  void before_market(TimestampNs timestamp_ns, std::uint64_t factual_sequence,
                     const replay::FactualState &state);
  void after_market(const itch::Message &message, std::uint64_t factual_sequence,
                    const replay::FactualState &state);
  [[nodiscard]] ShadowOrderId manual_submit(std::string symbol, ShadowSide side,
                                            std::uint32_t price4, std::uint64_t quantity,
                                            TimestampNs decision_timestamp_ns);
  [[nodiscard]] bool manual_cancel(ShadowOrderId order_id, TimestampNs decision_timestamp_ns);
  [[nodiscard]] bool manual_replace(ShadowOrderId order_id, std::uint32_t new_price4,
                                    std::uint64_t new_quantity, TimestampNs decision_timestamp_ns);
  void finish(TimestampNs timestamp_ns, const replay::FactualState &state);

  [[nodiscard]] const SimulationConfig &config() const noexcept { return config_; }
  [[nodiscard]] const std::map<ShadowOrderId, ShadowOrder> &orders() const noexcept {
    return orders_;
  }
  [[nodiscard]] const std::vector<ShadowOrderEvent> &order_events() const noexcept {
    return order_events_;
  }
  [[nodiscard]] const std::vector<ShadowFill> &fills() const noexcept { return fills_; }
  [[nodiscard]] const std::vector<InventoryEvent> &inventory_events() const noexcept {
    return inventory_events_;
  }
  [[nodiscard]] const AccountingLedger &ledger() const noexcept { return ledger_; }
  [[nodiscard]] bool stop_switch() const noexcept { return stop_switch_; }
  [[nodiscard]] bool check_invariants(std::string *error = nullptr) const;
  [[nodiscard]] std::string canonical_state() const;
  [[nodiscard]] std::string semantic_digest() const;
  [[nodiscard]] SimulationSummary summary() const;

private:
  enum class ScheduledKind : std::uint8_t {
    Observation,
    Decision,
    Activate,
    CancelConfirm,
    ReplaceConfirm
  };
  struct ScheduledEvent {
    TimestampNs timestamp_ns{};
    std::uint64_t factual_sequence{};
    ScheduledKind kind{ScheduledKind::Observation};
    LocalSequence local_sequence{};
    ShadowOrderId order_id{};
    std::uint32_t price4{};
    std::uint64_t quantity{};
    MarketSnapshot market;
  };
  struct ScheduledLess {
    [[nodiscard]] bool operator()(const ScheduledEvent &left,
                                  const ScheduledEvent &right) const noexcept;
  };
  struct MarketSample {
    TimestampNs timestamp_ns{};
    std::uint64_t factual_sequence{};
    bool valid{};
    std::int64_t mid2{};
    std::uint32_t bid_price4{};
    std::uint32_t ask_price4{};
  };

  SimulationConfig config_;
  MarketMakerStrategy strategy_;
  AccountingLedger ledger_;
  std::map<ShadowOrderId, ShadowOrder> orders_;
  std::map<ShadowOrderId, QueueTracker> queue_trackers_;
  std::multiset<ScheduledEvent, ScheduledLess> scheduled_;
  std::vector<ShadowOrderEvent> order_events_;
  std::vector<ShadowFill> fills_;
  std::vector<InventoryEvent> inventory_events_;
  std::map<std::string, MarketSnapshot> latest_markets_;
  std::map<std::string, std::vector<MarketSample>> market_samples_;
  ShadowOrderId next_order_id_{1};
  LocalSequence next_local_sequence_{1};
  LocalSequence next_priority_sequence_{1};
  std::uint64_t factual_events_{};
  std::uint64_t submitted_orders_{};
  std::uint64_t submitted_quantity_{};
  std::uint64_t acknowledged_orders_{};
  std::uint64_t rejected_orders_{};
  std::uint64_t cancelled_orders_{};
  std::uint64_t replaced_orders_{};
  std::uint64_t filled_orders_{};
  std::uint64_t risk_suppressions_{};
  std::uint64_t stop_switch_triggers_{};
  std::int64_t maximum_absolute_inventory_{};
  MoneyNanos peak_net_equity_nanos_{};
  MoneyNanos maximum_drawdown_nanos_{};
  bool stop_switch_{};
  std::optional<TimestampNs> last_factual_timestamp_;
  std::optional<std::uint64_t> last_factual_sequence_;
  std::optional<TimestampNs> last_accounting_timestamp_;
  long double absolute_inventory_time_integral_{};
  long double squared_inventory_time_integral_{};
  std::uint64_t quoted_time_ns_{};
  std::uint64_t eligible_market_time_ns_{};
  bool last_market_eligible_{};
  bool last_market_quoted_{};

  [[nodiscard]] static std::uint8_t category(ScheduledKind kind) noexcept;
  void schedule(ScheduledEvent event);
  void drain_before(TimestampNs timestamp_ns, const replay::FactualState &state);
  void drain_through(TimestampNs timestamp_ns, const replay::FactualState &state);
  void execute_scheduled(const ScheduledEvent &event, const replay::FactualState &state);
  void execute_observation(const ScheduledEvent &event);
  void execute_decision(const ScheduledEvent &event);
  void activate(ShadowOrderId order_id, TimestampNs timestamp_ns,
                const replay::FactualState &state);
  void confirm_cancel(ShadowOrderId order_id, TimestampNs timestamp_ns);
  void confirm_replace(const ScheduledEvent &event, const replay::FactualState &state);
  void manage_quote(const MarketSnapshot &market, ShadowSide side,
                    std::optional<std::uint32_t> desired_price, std::uint64_t quantity,
                    AuditReason suppression_reason, TimestampNs decision_timestamp_ns);
  [[nodiscard]] std::optional<ShadowOrderId> open_order(const std::string &symbol,
                                                        ShadowSide side) const;
  [[nodiscard]] AuditReason activation_risk(const ShadowOrder &order, TimestampNs timestamp_ns,
                                            const replay::FactualState &state) const;
  void apply_market_mutation(const replay::BookMutation &mutation, TimestampNs timestamp_ns,
                             std::uint64_t factual_sequence);
  void apply_fill(ShadowOrder &order, std::uint64_t quantity, const replay::BookMutation &mutation,
                  TimestampNs timestamp_ns, std::uint64_t factual_sequence, AuditReason reason);
  void record_order_event(ShadowOrder &order, ShadowOrderState from, ShadowOrderState to,
                          AuditReason reason, TimestampNs timestamp_ns);
  [[nodiscard]] bool transition(ShadowOrder &order, ShadowOrderState to, AuditReason reason,
                                TimestampNs timestamp_ns);
  void update_market(const itch::Message &message, std::uint64_t factual_sequence,
                     const replay::FactualState &state);
  [[nodiscard]] MarketSnapshot snapshot_for(const std::string &symbol, TimestampNs timestamp_ns,
                                            std::uint64_t factual_sequence,
                                            const replay::FactualState &state) const;
  void update_risk(TimestampNs timestamp_ns);
  void trigger_stop(AuditReason reason, TimestampNs timestamp_ns);
  void force_cancel_all(AuditReason reason, TimestampNs timestamp_ns);
  void accumulate_time(TimestampNs timestamp_ns);
  [[nodiscard]] bool is_open_state(ShadowOrderState state) const noexcept;
  [[nodiscard]] std::vector<MarkoutResult> calculate_markouts() const;
};

} // namespace lob::mm
