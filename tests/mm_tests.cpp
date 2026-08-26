#include "lob/mm/accounting.hpp"
#include "lob/mm/audit.hpp"
#include "lob/mm/protocol.hpp"
#include "lob/mm/queue_model.hpp"
#include "lob/mm/simulator.hpp"
#include "lob/mm/strategies.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace lob;

class Failure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      throw Failure(std::string{"check failed: "} + #expression);                                  \
    }                                                                                              \
  } while (false)

template <std::size_t Size> itch::FixedAscii<Size> fixed(const std::string_view value) {
  itch::FixedAscii<Size> result;
  result.raw.fill(' ');
  std::copy_n(value.begin(), std::min(value.size(), Size), result.raw.begin());
  return result;
}

itch::CommonHeader header(const itch::StockLocate locate, const std::uint64_t timestamp) {
  return {locate, 1, timestamp};
}

itch::StockDirectory directory(const itch::StockLocate locate, const std::string_view symbol,
                               const std::uint64_t timestamp) {
  itch::StockDirectory value{};
  value.header = header(locate, timestamp);
  value.stock = fixed<8>(symbol);
  value.market_category = 'Q';
  value.financial_status = 'N';
  value.round_lot_size = 100;
  value.round_lots_only = 'N';
  value.issue_classification = 'C';
  value.issue_sub_type = fixed<2>("");
  value.authenticity = 'P';
  value.short_sale_threshold_indicator = 'N';
  value.ipo_flag = 'N';
  value.luld_reference_price_tier = '1';
  value.etp_flag = 'N';
  value.etp_leverage_factor = 1;
  value.inverse_indicator = 'N';
  return value;
}

itch::AddOrder add(const itch::StockLocate locate, const std::string_view symbol,
                   const itch::OrderReference reference, const itch::FeedSide side,
                   const std::uint32_t shares, const itch::Price4 price,
                   const std::uint64_t timestamp) {
  return {header(locate, timestamp), reference, side, shares, fixed<8>(symbol), price};
}

void apply_ok(replay::FactualState &state, const itch::Message &message) {
  const auto result = state.apply(message);
  CHECK(result.ok());
  CHECK(state.check_invariants());
}

mm::SimulationConfig manual_config() {
  mm::SimulationConfig config;
  config.automatic_strategy = false;
  config.latency = {};
  config.latency.cancel_ns = 10;
  config.latency.replace_ns = 10;
  config.queue_model = mm::QueueModel::MboFifoConservative;
  config.quote.tick_size_price4 = 1;
  config.risk.max_absolute_inventory = 100;
  config.risk.max_order_quantity = 100;
  config.risk.max_open_orders = 10;
  config.risk.max_notional_nanos = 1'000'000'000'000LL;
  config.risk.max_market_staleness_ns = 1'000;
  config.risk.stop_new_quotes_before_close_ns = 0;
  config.strategy.session_end_ns = 1'000'000;
  config.markout_horizons_ns = {10, 100, 1'000};
  config.protocol_sha256 = "test";
  return config;
}

void process(mm::ShadowSimulator &simulator, replay::FactualState &state,
             const itch::Message &message, std::uint64_t &sequence) {
  ++sequence;
  const auto timestamp = itch::common_header(message).timestamp;
  simulator.before_market(timestamp, sequence, state);
  apply_ok(state, message);
  simulator.after_market(message, sequence, state);
  CHECK(simulator.check_invariants());
}

void initialize_market(mm::ShadowSimulator &simulator, replay::FactualState &state,
                       std::uint64_t &sequence, const std::string_view symbol = "TEST",
                       const itch::StockLocate locate = 1) {
  state.capture_book_mutations(true);
  process(simulator, state, itch::Message{itch::SystemEvent{header(0, 1), 'O'}}, sequence);
  process(simulator, state, itch::Message{itch::SystemEvent{header(0, 2), 'S'}}, sequence);
  process(simulator, state, itch::Message{directory(locate, symbol, 3)}, sequence);
  process(simulator, state, itch::Message{itch::SystemEvent{header(0, 4), 'Q'}}, sequence);
  process(simulator, state,
          itch::Message{itch::StockTradingAction{header(locate, 5), fixed<8>(symbol), 'T', ' ',
                                                 fixed<4>("")}},
          sequence);
  process(simulator, state, itch::Message{add(locate, symbol, 10, itch::FeedSide::Buy, 10, 100, 6)},
          sequence);
  process(simulator, state,
          itch::Message{add(locate, symbol, 11, itch::FeedSide::Sell, 10, 102, 7)}, sequence);
}

void sha256_and_protocol() {
  CHECK(mm::sha256_hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const auto protocol =
      mm::load_protocol(std::filesystem::path{LOB_SOURCE_DIR} / "configs/round4_protocol.toml");
  CHECK(protocol.sha256.size() == 64);
  CHECK(protocol.simulation.protocol_sha256 == protocol.sha256);
  CHECK(protocol.simulation.strategy.fitted_partition == "train");
  CHECK(protocol.simulation.strategy.fitted_protocol_sha256 == protocol.sha256);
  CHECK(protocol.parsed_values.contains("ordering.timestamp_tie_rule"));
}

void accounting_exactness_and_overflow() {
  mm::AccountingLedger ledger;
  mm::FeeConfig fees{2, 1, 3};
  auto delta = ledger.apply_fill(mm::ShadowSide::Buy, 10, 100, fees);
  CHECK(delta.has_value());
  CHECK(ledger.inventory() == 10);
  CHECK(ledger.trade_cash_nanos() == -100'000'000);
  CHECK(ledger.fees_nanos() == 20 && ledger.rebates_nanos() == 10);
  delta = ledger.apply_fill(mm::ShadowSide::Sell, 4, 110, fees);
  CHECK(delta.has_value() && delta->realized_gross_pnl_nanos == 4'000'000);
  CHECK(ledger.inventory() == 6 && ledger.turnover_quantity() == 14);
  CHECK(ledger.trade_cash_nanos() == -56'000'000);
  CHECK(ledger.gross_equity_at_mid2(220) == 10'000'000);
  CHECK(ledger.net_equity_at_mid2(220) == 9'999'986);
  CHECK(ledger.conservative_liquidation_equity(105, 115, 3) == 6'999'968);
  CHECK(!ledger.gross_equity_at_mid2(-1).has_value());
  CHECK(ledger.check_invariants());

  mm::AccountingLedger overflow;
  mm::FeeConfig excessive{std::numeric_limits<mm::MoneyNanos>::max(), 0, 0};
  CHECK(!overflow.apply_fill(mm::ShadowSide::Buy, 2, 1, excessive).has_value());
  CHECK(overflow.inventory() == 0 && overflow.trade_cash_nanos() == 0);
}

void strategy_math_and_boundaries() {
  mm::MarketSnapshot market;
  market.exchange_timestamp_ns = 1'000'000'000;
  market.session_phase = replay::SessionPhase::MarketHours;
  market.trading_state = 'T';
  market.bids = {{100, 20, 1}, {99, 20, 1}, {98, 20, 1}};
  market.asks = {{102, 20, 1}, {103, 20, 1}, {104, 20, 1}};
  market.mid2 = 202;
  market.two_sided = true;
  market.imbalance_l1 = 0.0;
  auto config = manual_config();
  config.automatic_strategy = true;
  config.quote.quantity = 5;
  config.quote.maximum_distance_ticks = 10;
  config.strategy.session_end_ns = 2'000'000'000;
  config.strategy.symmetric_half_spread_price4 = 1.0;

  const auto symmetric =
      mm::MarketMakerStrategy{mm::StrategyKind::SymmetricQuote}.quote(market, 0, config);
  CHECK(symmetric.bid_price4 == 100 && symmetric.ask_price4 == 102);
  CHECK(symmetric.reservation_price4 == 101.0 && symmetric.total_spread_price4 == 2.0);

  config.strategy.gamma = 0.1;
  config.strategy.sigma_squared = 0.01;
  config.strategy.arrival_intensity_k = 2.0;
  const auto as =
      mm::MarketMakerStrategy{mm::StrategyKind::AvellanedaStoikov}.quote(market, 10, config);
  CHECK(as.reservation_price4.has_value());
  CHECK(std::fabs(*as.reservation_price4 - 100.99) < 1e-12);
  const auto signal =
      mm::MarketMakerStrategy{mm::StrategyKind::SignalAwareAs}.quote(market, 10, config);
  CHECK(signal.reservation_price4 == as.reservation_price4);
  market.causal_signal = 2.0;
  config.strategy.signal_coefficient_price4 = 0.5;
  const auto shifted =
      mm::MarketMakerStrategy{mm::StrategyKind::SignalAwareAs}.quote(market, 10, config);
  CHECK(std::fabs(*shifted.reservation_price4 - (*as.reservation_price4 + 1.0)) < 1e-12);

  config.risk.max_absolute_inventory = 10;
  const auto limited =
      mm::MarketMakerStrategy{mm::StrategyKind::SymmetricQuote}.quote(market, 10, config);
  CHECK(!limited.bid_price4.has_value());
  CHECK(limited.bid_reason == mm::AuditReason::InventorySideSuppressed);
  market.crossed = true;
  CHECK(!mm::MarketMakerStrategy{mm::StrategyKind::SymmetricQuote}
             .quote(market, 0, config)
             .ask_price4.has_value());
  market.crossed = false;
  config.strategy.gamma = 0.0;
  CHECK(!mm::MarketMakerStrategy{mm::StrategyKind::AvellanedaStoikov}
             .quote(market, 0, config)
             .bid_price4.has_value());
}

replay::FactualState queue_state() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "TEST", 1)});
  for (std::uint64_t reference = 1; reference <= 100; ++reference) {
    apply_ok(state, itch::Message{
                        add(1, "TEST", reference, itch::FeedSide::Buy, 1'000, 100, reference + 1)});
  }
  return state;
}

void queue_golden_models() {
  auto state = queue_state();
  const replay::BookMutation same_price{'E',          1, "TEST", itch::FeedSide::Buy, 500,
                                        std::nullopt, 7, 100,    std::nullopt,        1};
  const replay::BookMutation worse_price{'E',          1, "TEST", itch::FeedSide::Buy, 501,
                                         std::nullopt, 9, 99,     std::nullopt,        2};
  mm::QueueTracker primary(mm::QueueModel::MboFifoConservative, mm::ShadowSide::Buy, 100);
  primary.snapshot(state, "TEST");
  CHECK(primary.quantity_ahead() == 100'000);
  CHECK(primary.on_mutation(same_price).fillable_quantity == 0);
  const auto trade_through = primary.on_mutation(worse_price);
  CHECK(trade_through.fillable_quantity == 9 &&
        trade_through.reason == mm::AuditReason::TradeThrough);

  mm::QueueTracker pessimistic(mm::QueueModel::TradeThroughOnly, mm::ShadowSide::Buy, 100);
  pessimistic.snapshot(state, "TEST");
  CHECK(pessimistic.on_mutation(same_price).fillable_quantity == 0);
  CHECK(pessimistic.on_mutation(worse_price).fillable_quantity == 9);

  mm::QueueTracker optimistic(mm::QueueModel::FrontOfQueue, mm::ShadowSide::Buy, 100);
  optimistic.snapshot(state, "TEST");
  CHECK(optimistic.on_mutation(same_price).fillable_quantity == 7);
}

void queue_random_oracle_100k() {
  std::uint64_t total_events = 0;
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    auto state = queue_state();
    mm::QueueTracker tracker(mm::QueueModel::MboFifoConservative, mm::ShadowSide::Buy, 100);
    tracker.snapshot(state, "TEST");
    std::map<std::uint64_t, std::uint64_t> oracle;
    for (std::uint64_t reference = 1; reference <= 100; ++reference) {
      oracle.emplace(reference, 1'000);
    }
    std::uint64_t oracle_quantity = 100'000;
    std::mt19937_64 random(seed);
    for (std::uint64_t event_index = 0; event_index < 5'000; ++event_index) {
      const auto selector = random() % 8;
      const auto reference = 1 + random() % 150;
      const auto quantity = 1 + random() % 5;
      char type = selector < 3    ? 'X'
                  : selector < 5  ? 'E'
                  : selector == 5 ? 'C'
                  : selector == 6 ? 'D'
                                  : 'U';
      const auto price = selector == 4 ? 99U : 100U;
      replay::BookMutation mutation{
          type,
          1,
          "TEST",
          itch::FeedSide::Buy,
          reference,
          type == 'U' ? std::optional<std::uint64_t>{1'000 + reference} : std::nullopt,
          quantity,
          price,
          type == 'C' ? std::optional<std::uint32_t>{101} : std::nullopt,
          (type == 'E' || type == 'C') ? std::optional<std::uint64_t>{event_index + 1}
                                       : std::nullopt};
      std::uint64_t expected_fill = 0;
      if ((type == 'E' || type == 'C') && price < 100) {
        expected_fill = quantity;
      } else if (auto position = oracle.find(reference); position != oracle.end()) {
        if (type == 'D' || type == 'U') {
          oracle_quantity -= position->second;
          oracle.erase(position);
        } else {
          const auto removed = std::min(position->second, quantity);
          position->second -= removed;
          oracle_quantity -= removed;
          if (position->second == 0) {
            oracle.erase(position);
          }
        }
      } else if ((type == 'E' || type == 'C') && oracle_quantity == 0) {
        expected_fill = quantity;
      }
      const auto actual = tracker.on_mutation(mutation);
      if (actual.fillable_quantity != expected_fill ||
          tracker.quantity_ahead() != oracle_quantity || tracker.orders_ahead() != oracle ||
          !tracker.check_invariants()) {
        std::ostringstream reproducer;
        reproducer << "queue oracle mismatch seed=" << seed
                   << " prefix_events=" << (event_index + 1) << " type=" << type
                   << " reference=" << reference << " quantity=" << quantity << " price4=" << price
                   << " expected_fill=" << expected_fill
                   << " actual_fill=" << actual.fillable_quantity
                   << " expected_ahead=" << oracle_quantity
                   << " actual_ahead=" << tracker.quantity_ahead();
        throw Failure(reproducer.str());
      }
      ++total_events;
    }
  }
  CHECK(total_events == 100'000);
}

void simulator_fifo_cancel_latency_and_boundaries() {
  auto config = manual_config();
  replay::FactualState state;
  mm::ShadowSimulator simulator(config);
  std::uint64_t sequence = 0;
  initialize_market(simulator, state, sequence);
  const auto factual_before = state.canonical_state();
  const auto order_id = simulator.manual_submit("TEST", mm::ShadowSide::Buy, 100, 10, 7);
  CHECK(state.canonical_state() == factual_before);
  process(simulator, state,
          itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}}, sequence);
  CHECK(simulator.orders().at(order_id).state == mm::ShadowOrderState::Active);
  CHECK(simulator.orders().at(order_id).queue_ahead_quantity == 10);
  process(simulator, state, itch::Message{add(1, "TEST", 12, itch::FeedSide::Buy, 10, 100, 9)},
          sequence);
  CHECK(simulator.orders().at(order_id).queue_ahead_quantity == 10);
  process(simulator, state, itch::Message{itch::OrderExecuted{header(1, 10), 10, 10, 1}}, sequence);
  CHECK(simulator.fills().empty());
  CHECK(simulator.orders().at(order_id).queue_ahead_quantity == 0);
  process(simulator, state, itch::Message{itch::OrderExecuted{header(1, 11), 12, 5, 2}}, sequence);
  CHECK(simulator.fills().size() == 1 && simulator.ledger().inventory() == 5);
  CHECK(simulator.orders().at(order_id).state == mm::ShadowOrderState::PartiallyFilled);
  CHECK(simulator.manual_cancel(order_id, 11));
  CHECK(simulator.orders().at(order_id).state == mm::ShadowOrderState::PendingCancel);
  process(simulator, state,
          itch::Message{itch::OrderExecutedWithPrice{header(1, 16), 12, 5, 3, 'Y', 101}}, sequence);
  CHECK(simulator.fills().size() == 2 && simulator.ledger().inventory() == 10);
  CHECK(simulator.orders().at(order_id).state == mm::ShadowOrderState::Filled);
  CHECK(simulator.fills().back().accounting_price4 == 100);
  CHECK(simulator.fills().back().factual_display_price4 == 100);
  CHECK(simulator.fills().back().factual_execution_price4 == 101);
  CHECK(!simulator.manual_cancel(order_id, 17));
  simulator.finish(20, state);
  CHECK(simulator.check_invariants());
}

void simulator_all_latencies_cancel_and_multisymbol() {
  auto latency_config = manual_config();
  latency_config.automatic_strategy = true;
  latency_config.strategy_kind = mm::StrategyKind::SymmetricQuote;
  latency_config.latency.market_data_ns = 10;
  latency_config.latency.strategy_compute_ns = 20;
  latency_config.latency.order_entry_ns = 30;
  latency_config.quote.quantity = 5;
  latency_config.quote.minimum_rest_ns = 1'000;
  latency_config.strategy.symmetric_half_spread_price4 = 1.0;
  replay::FactualState latency_state;
  mm::ShadowSimulator latency_simulator(latency_config);
  std::uint64_t latency_sequence = 0;
  initialize_market(latency_simulator, latency_state, latency_sequence);
  process(latency_simulator, latency_state,
          itch::Message{itch::RegShoRestriction{header(1, 68), fixed<8>("TEST"), '0'}},
          latency_sequence);
  CHECK(latency_simulator.orders().size() == 2);
  for (const auto &[order_id, order] : latency_simulator.orders()) {
    (void)order_id;
    CHECK(order.submit_timestamp_ns == 37);
    CHECK(order.active_timestamp_ns == 67);
    CHECK(order.state == mm::ShadowOrderState::Active);
  }
  CHECK(latency_simulator.order_events()[0].timestamp_ns == 37);
  CHECK(latency_simulator.order_events()[2].timestamp_ns == 37);
  CHECK(latency_simulator.order_events()[4].timestamp_ns == 67);
  CHECK(latency_simulator.order_events()[5].timestamp_ns == 67);

  auto cancel_config = manual_config();
  replay::FactualState cancel_state;
  mm::ShadowSimulator cancel_simulator(cancel_config);
  std::uint64_t cancel_sequence = 0;
  initialize_market(cancel_simulator, cancel_state, cancel_sequence);
  const auto cancelled = cancel_simulator.manual_submit("TEST", mm::ShadowSide::Buy, 100, 5, 7);
  process(cancel_simulator, cancel_state,
          itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}},
          cancel_sequence);
  CHECK(cancel_simulator.manual_cancel(cancelled, 8));
  CHECK(cancel_simulator.orders().at(cancelled).state == mm::ShadowOrderState::PendingCancel);
  process(cancel_simulator, cancel_state,
          itch::Message{itch::RegShoRestriction{header(1, 19), fixed<8>("TEST"), '0'}},
          cancel_sequence);
  CHECK(cancel_simulator.orders().at(cancelled).state == mm::ShadowOrderState::Cancelled);
  CHECK(cancel_simulator.orders().at(cancelled).terminal_timestamp_ns == 18);
  CHECK(cancel_simulator.order_events().back().reason == mm::AuditReason::CancelConfirmed);

  auto multi_config = manual_config();
  replay::FactualState multi_state;
  mm::ShadowSimulator multi_simulator(multi_config);
  std::uint64_t multi_sequence = 0;
  initialize_market(multi_simulator, multi_state, multi_sequence);
  process(multi_simulator, multi_state, itch::Message{directory(2, "TWO", 8)}, multi_sequence);
  process(multi_simulator, multi_state,
          itch::Message{
              itch::StockTradingAction{header(2, 8), fixed<8>("TWO"), 'T', ' ', fixed<4>("")}},
          multi_sequence);
  process(multi_simulator, multi_state,
          itch::Message{add(2, "TWO", 20, itch::FeedSide::Buy, 10, 200, 9)}, multi_sequence);
  process(multi_simulator, multi_state,
          itch::Message{add(2, "TWO", 21, itch::FeedSide::Sell, 10, 202, 9)}, multi_sequence);
  const auto first = multi_simulator.manual_submit("TEST", mm::ShadowSide::Buy, 100, 5, 9);
  const auto second = multi_simulator.manual_submit("TWO", mm::ShadowSide::Sell, 202, 5, 9);
  process(multi_simulator, multi_state,
          itch::Message{itch::RegShoRestriction{header(1, 9), fixed<8>("TEST"), '0'}},
          multi_sequence);
  CHECK(multi_simulator.orders().at(first).state == mm::ShadowOrderState::PendingAck);
  CHECK(multi_simulator.orders().at(second).state == mm::ShadowOrderState::PendingAck);
  process(multi_simulator, multi_state,
          itch::Message{itch::RegShoRestriction{header(2, 10), fixed<8>("TWO"), '0'}},
          multi_sequence);
  CHECK(multi_simulator.orders().at(first).state == mm::ShadowOrderState::Active);
  CHECK(multi_simulator.orders().at(second).state == mm::ShadowOrderState::Active);
  CHECK(multi_simulator.orders().at(first).queue_ahead_quantity == 10);
  CHECK(multi_simulator.orders().at(second).queue_ahead_quantity == 10);
  CHECK(multi_simulator.check_invariants());
}

void simulator_tie_reject_replace_and_session_end() {
  auto config = manual_config();
  replay::FactualState state;
  mm::ShadowSimulator simulator(config);
  std::uint64_t sequence = 0;
  initialize_market(simulator, state, sequence);
  const auto crossing = simulator.manual_submit("TEST", mm::ShadowSide::Buy, 102, 5, 7);
  process(simulator, state,
          itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}}, sequence);
  CHECK(simulator.orders().at(crossing).state == mm::ShadowOrderState::Rejected);
  CHECK(simulator.order_events().back().reason == mm::AuditReason::CrossesBook);

  const auto passive = simulator.manual_submit("TEST", mm::ShadowSide::Sell, 102, 5, 8);
  process(simulator, state,
          itch::Message{itch::RegShoRestriction{header(1, 9), fixed<8>("TEST"), '0'}}, sequence);
  const auto old_priority = simulator.orders().at(passive).priority_sequence;
  CHECK(simulator.manual_replace(passive, 102, 5, 9));
  process(simulator, state,
          itch::Message{itch::RegShoRestriction{header(1, 20), fixed<8>("TEST"), '0'}}, sequence);
  CHECK(simulator.orders().at(passive).priority_sequence > old_priority);
  CHECK(simulator.orders().at(passive).queue_ahead_quantity == 10);
  CHECK(simulator.orders().at(passive).active_timestamp_ns == 19);
  CHECK(simulator.order_events().back().reason == mm::AuditReason::ReplaceConfirmed);
  CHECK(simulator.order_events().back().timestamp_ns == 19);
  process(simulator, state, itch::Message{itch::SystemEvent{header(0, 21), 'M'}}, sequence);
  CHECK(simulator.orders().at(passive).state == mm::ShadowOrderState::SessionEnded);

  auto tie_config = manual_config();
  replay::FactualState tie_state;
  mm::ShadowSimulator tie(tie_config);
  std::uint64_t tie_sequence = 0;
  initialize_market(tie, tie_state, tie_sequence);
  const auto tie_order = tie.manual_submit("TEST", mm::ShadowSide::Buy, 100, 5, 7);
  process(tie, tie_state, itch::Message{itch::OrderExecuted{header(1, 7), 10, 5, 99}},
          tie_sequence);
  CHECK(tie.orders().at(tie_order).state == mm::ShadowOrderState::PendingAck);
  CHECK(tie.fills().empty());
  process(tie, tie_state,
          itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}},
          tie_sequence);
  CHECK(tie.orders().at(tie_order).queue_ahead_quantity == 5);
}

void simulator_determinism_and_markout() {
  std::string first_digest;
  for (int run = 0; run < 10; ++run) {
    auto config = manual_config();
    replay::FactualState state;
    mm::ShadowSimulator simulator(config);
    std::uint64_t sequence = 0;
    initialize_market(simulator, state, sequence);
    const auto order = simulator.manual_submit("TEST", mm::ShadowSide::Buy, 100, 5, 7);
    process(simulator, state,
            itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}}, sequence);
    process(simulator, state, itch::Message{itch::OrderDelete{header(1, 9), 10}}, sequence);
    process(simulator, state, itch::Message{add(1, "TEST", 12, itch::FeedSide::Buy, 5, 100, 10)},
            sequence);
    process(simulator, state, itch::Message{add(1, "TEST", 15, itch::FeedSide::Buy, 5, 100, 10)},
            sequence);
    process(simulator, state, itch::Message{itch::OrderExecuted{header(1, 11), 12, 5, 101}},
            sequence);
    CHECK(simulator.orders().at(order).state == mm::ShadowOrderState::Filled);
    process(simulator, state, itch::Message{itch::OrderReplace{header(1, 21), 11, 13, 10, 104}},
            sequence);
    process(simulator, state, itch::Message{add(1, "TEST", 14, itch::FeedSide::Buy, 10, 102, 111)},
            sequence);
    process(simulator, state,
            itch::Message{itch::RegShoRestriction{header(1, 1'011), fixed<8>("TEST"), '0'}},
            sequence);
    simulator.finish(1'011, state);
    const auto summary = simulator.summary();
    CHECK(summary.markouts.size() == 3);
    CHECK(summary.markouts[0].eligible_fills == 1);
    CHECK(summary.markouts[0].eligible_quantity == 5);
    CHECK(summary.markouts[0].directional_value_nanos == 1'000'000);
    CHECK(summary.markouts[1].directional_value_nanos == 1'500'000);
    CHECK(summary.markouts[2].directional_value_nanos == 1'500'000);
    if (run == 0) {
      first_digest = summary.semantic_digest;
    }
    CHECK(summary.semantic_digest == first_digest);
  }
}

void simulator_prefix_invariance() {
  const auto run = [](const bool filling_tail) {
    auto config = manual_config();
    replay::FactualState state;
    mm::ShadowSimulator simulator(config);
    std::uint64_t sequence = 0;
    initialize_market(simulator, state, sequence);
    (void)simulator.manual_submit("TEST", mm::ShadowSide::Buy, 100, 5, 7);
    process(simulator, state,
            itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}}, sequence);
    if (filling_tail) {
      process(simulator, state, itch::Message{itch::OrderExecuted{header(1, 9), 10, 10, 1}},
              sequence);
      process(simulator, state, itch::Message{add(1, "TEST", 12, itch::FeedSide::Buy, 5, 100, 10)},
              sequence);
      process(simulator, state, itch::Message{itch::OrderExecuted{header(1, 11), 12, 5, 2}},
              sequence);
    } else {
      process(simulator, state, itch::Message{itch::OrderDelete{header(1, 9), 10}}, sequence);
      process(simulator, state, itch::Message{add(1, "TEST", 12, itch::FeedSide::Buy, 5, 99, 10)},
              sequence);
    }
    simulator.finish(20, state);
    std::string prefix;
    for (const auto &event : simulator.order_events()) {
      if (event.timestamp_ns <= 8) {
        prefix += mm::render_json(event);
        prefix.push_back('\n');
      }
    }
    for (const auto &fill : simulator.fills()) {
      if (fill.timestamp_ns <= 8) {
        prefix += mm::render_json(fill);
        prefix.push_back('\n');
      }
    }
    return std::pair{prefix, simulator.semantic_digest()};
  };
  const auto no_fill = run(false);
  const auto fill = run(true);
  CHECK(no_fill.first == fill.first);
  CHECK(no_fill.second != fill.second);
}

void simulator_risk_limits_and_stop_switch() {
  const auto rejection = [](mm::SimulationConfig config, const std::uint32_t price,
                            const std::uint64_t quantity, const std::uint64_t decision_time,
                            const mm::AuditReason expected) {
    replay::FactualState state;
    mm::ShadowSimulator simulator(config);
    std::uint64_t sequence = 0;
    initialize_market(simulator, state, sequence);
    const auto order =
        simulator.manual_submit("TEST", mm::ShadowSide::Buy, price, quantity, decision_time);
    process(
        simulator, state,
        itch::Message{itch::RegShoRestriction{header(1, decision_time + 1), fixed<8>("TEST"), '0'}},
        sequence);
    CHECK(simulator.orders().at(order).state == mm::ShadowOrderState::Rejected);
    CHECK(simulator.order_events().back().reason == expected);
  };

  auto quantity = manual_config();
  quantity.risk.max_order_quantity = 4;
  rejection(quantity, 100, 5, 7, mm::AuditReason::QuantityLimit);
  auto inventory = manual_config();
  inventory.risk.max_absolute_inventory = 4;
  rejection(inventory, 100, 5, 7, mm::AuditReason::InventoryLimit);
  auto notional = manual_config();
  notional.risk.max_notional_nanos = 49'999'999;
  rejection(notional, 100, 5, 7, mm::AuditReason::NotionalLimit);
  auto stale = manual_config();
  stale.risk.max_market_staleness_ns = 10;
  rejection(stale, 100, 5, 100, mm::AuditReason::StaleMarket);
  auto close = manual_config();
  close.strategy.session_end_ns = 100;
  close.risk.stop_new_quotes_before_close_ns = 20;
  rejection(close, 100, 5, 81, mm::AuditReason::CloseCutoff);

  const auto state_rejection = [](const char scenario, const mm::AuditReason expected) {
    auto config = manual_config();
    replay::FactualState state;
    mm::ShadowSimulator simulator(config);
    std::uint64_t sequence = 0;
    initialize_market(simulator, state, sequence);
    std::uint64_t decision = 8;
    if (scenario == 'H') {
      process(simulator, state,
              itch::Message{
                  itch::StockTradingAction{header(1, 8), fixed<8>("TEST"), 'H', ' ', fixed<4>("")}},
              sequence);
    } else if (scenario == 'S') {
      process(simulator, state, itch::Message{itch::SystemEvent{header(0, 8), 'M'}}, sequence);
    } else if (scenario == 'O') {
      process(simulator, state, itch::Message{itch::OrderDelete{header(1, 8), 11}}, sequence);
    } else if (scenario == 'L') {
      process(simulator, state, itch::Message{itch::OrderReplace{header(1, 8), 11, 13, 10, 100}},
              sequence);
    }
    const std::string symbol = scenario == 'M' ? "UNKNOWN" : "TEST";
    const std::uint32_t price = scenario == 'L' ? 99 : 100;
    const auto order = simulator.manual_submit(symbol, mm::ShadowSide::Buy, price, 5, decision);
    process(simulator, state,
            itch::Message{itch::RegShoRestriction{header(1, 9), fixed<8>("TEST"), '0'}}, sequence);
    CHECK(simulator.orders().at(order).state == mm::ShadowOrderState::Rejected);
    CHECK(simulator.order_events().back().reason == expected);
  };
  state_rejection('H', mm::AuditReason::TradingHalt);
  state_rejection('S', mm::AuditReason::SessionClosed);
  state_rejection('O', mm::AuditReason::MissingBook);
  state_rejection('L', mm::AuditReason::LockedOrCrossed);
  state_rejection('M', mm::AuditReason::MissingBook);

  auto open_config = manual_config();
  open_config.risk.max_open_orders = 1;
  replay::FactualState open_state;
  mm::ShadowSimulator open_simulator(open_config);
  std::uint64_t open_sequence = 0;
  initialize_market(open_simulator, open_state, open_sequence);
  const auto first = open_simulator.manual_submit("TEST", mm::ShadowSide::Buy, 100, 5, 7);
  process(open_simulator, open_state,
          itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}},
          open_sequence);
  CHECK(open_simulator.orders().at(first).state == mm::ShadowOrderState::Active);
  const auto second = open_simulator.manual_submit("TEST", mm::ShadowSide::Sell, 102, 5, 8);
  process(open_simulator, open_state,
          itch::Message{itch::RegShoRestriction{header(1, 9), fixed<8>("TEST"), '0'}},
          open_sequence);
  CHECK(open_simulator.orders().at(second).state == mm::ShadowOrderState::Rejected);
  CHECK(open_simulator.order_events().back().reason == mm::AuditReason::OpenOrderLimit);

  const auto stop_case = [](const bool drawdown) {
    auto config = manual_config();
    config.risk.maximum_loss_nanos = drawdown ? 1'000'000'000 : 1'000'000;
    config.risk.maximum_drawdown_nanos = drawdown ? 1'000'000 : 1'000'000'000;
    replay::FactualState state;
    mm::ShadowSimulator simulator(config);
    std::uint64_t sequence = 0;
    initialize_market(simulator, state, sequence);
    const auto buy = simulator.manual_submit("TEST", mm::ShadowSide::Buy, 100, 5, 7);
    const auto sell = simulator.manual_submit("TEST", mm::ShadowSide::Sell, 102, 5, 7);
    process(simulator, state,
            itch::Message{itch::RegShoRestriction{header(1, 8), fixed<8>("TEST"), '0'}}, sequence);
    process(simulator, state, itch::Message{add(1, "TEST", 12, itch::FeedSide::Buy, 5, 100, 9)},
            sequence);
    process(simulator, state, itch::Message{add(1, "TEST", 13, itch::FeedSide::Buy, 5, 100, 10)},
            sequence);
    process(simulator, state, itch::Message{itch::OrderExecuted{header(1, 11), 10, 10, 1}},
            sequence);
    process(simulator, state, itch::Message{itch::OrderExecuted{header(1, 12), 12, 5, 2}},
            sequence);
    CHECK(simulator.orders().at(buy).state == mm::ShadowOrderState::Filled);
    CHECK(simulator.orders().at(sell).state == mm::ShadowOrderState::Active);
    process(simulator, state, itch::Message{itch::OrderDelete{header(1, 13), 13}}, sequence);
    process(simulator, state, itch::Message{itch::OrderReplace{header(1, 14), 11, 20, 10, 92}},
            sequence);
    process(simulator, state, itch::Message{add(1, "TEST", 21, itch::FeedSide::Buy, 10, 90, 15)},
            sequence);
    CHECK(simulator.stop_switch());
    CHECK(simulator.orders().at(sell).state == mm::ShadowOrderState::RiskCancelled);
    CHECK(simulator.order_events().back().reason ==
          (drawdown ? mm::AuditReason::MaximumDrawdown : mm::AuditReason::MaximumLoss));
    const auto rejected_after_stop =
        simulator.manual_submit("TEST", mm::ShadowSide::Sell, 92, 5, 15);
    process(simulator, state,
            itch::Message{itch::RegShoRestriction{header(1, 16), fixed<8>("TEST"), '0'}}, sequence);
    CHECK(simulator.orders().at(rejected_after_stop).state == mm::ShadowOrderState::Rejected);
    CHECK(simulator.order_events().back().reason == mm::AuditReason::StopSwitch);
  };
  stop_case(false);
  stop_case(true);
}

} // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"sha256_and_protocol", sha256_and_protocol},
      {"accounting_exactness_and_overflow", accounting_exactness_and_overflow},
      {"strategy_math_and_boundaries", strategy_math_and_boundaries},
      {"queue_golden_models", queue_golden_models},
      {"queue_random_oracle_100k", queue_random_oracle_100k},
      {"simulator_fifo_cancel_latency_and_boundaries",
       simulator_fifo_cancel_latency_and_boundaries},
      {"simulator_all_latencies_cancel_and_multisymbol",
       simulator_all_latencies_cancel_and_multisymbol},
      {"simulator_tie_reject_replace_and_session_end",
       simulator_tie_reject_replace_and_session_end},
      {"simulator_determinism_and_markout", simulator_determinism_and_markout},
      {"simulator_prefix_invariance", simulator_prefix_invariance},
      {"simulator_risk_limits_and_stop_switch", simulator_risk_limits_and_stop_switch},
  };
  std::size_t failures = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
    }
  }
  std::cout << "tests=" << tests.size() << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
