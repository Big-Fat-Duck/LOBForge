#include "lob/mm/strategies.hpp"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

std::uint64_t parse_rows(const int argc, char **argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--rows") {
    return 0;
  }
  char *end = nullptr;
  const auto value = std::strtoull(argv[2], &end, 10);
  return end != argv[2] && *end == '\0' && value != 0 ? value : 0;
}

} // namespace

int main(int argc, char **argv) {
  const auto rows = parse_rows(argc, argv);
  if (rows == 0) {
    std::cerr << "usage: mm_strategy_vectors --rows <positive-integer>\n";
    return 2;
  }
  std::cout << "index,kind,timestamp_ns,session_end_ns,inventory,gamma,sigma_squared,k,signal,"
               "signal_coefficient,bid,ask,reservation,total_spread\n";
  for (std::uint64_t index = 0; index < rows; ++index) {
    lob::mm::MarketSnapshot market;
    market.exchange_timestamp_ns = 1'000'000'000ULL + index * 10'000ULL;
    market.session_phase = lob::replay::SessionPhase::MarketHours;
    market.trading_state = 'T';
    market.two_sided = true;
    market.mid2 = 20'002;
    market.causal_signal = static_cast<double>(static_cast<std::int64_t>(index % 21) - 10) / 10.0;
    for (std::uint32_t level = 0; level <= 100; ++level) {
      market.bids.push_back({10'000U - level, 100U + level, 1});
      market.asks.push_back({10'002U + level, 100U + level, 1});
    }
    lob::mm::SimulationConfig config;
    config.quote.quantity = 10;
    config.quote.tick_size_price4 = 1;
    config.quote.maximum_distance_ticks = 100;
    config.risk.max_absolute_inventory = 1'000;
    config.strategy.session_end_ns =
        market.exchange_timestamp_ns + (1ULL + index % 100ULL) * 1'000'000'000ULL;
    config.strategy.symmetric_half_spread_price4 = 1.0 + static_cast<double>(index % 5) * 0.1;
    config.strategy.gamma = 0.001 + static_cast<double>(index % 17) * 0.0001;
    config.strategy.sigma_squared = 0.001 + static_cast<double>(index % 13) * 0.0002;
    config.strategy.arrival_intensity_k = 1.0 + static_cast<double>(index % 11) * 0.05;
    config.strategy.signal_coefficient_price4 = 0.05 + static_cast<double>(index % 7) * 0.01;
    const auto inventory = static_cast<std::int64_t>(index % 41) - 20;
    const auto kind = static_cast<lob::mm::StrategyKind>(index % 3);
    const auto quote = lob::mm::MarketMakerStrategy{kind}.quote(market, inventory, config);
    std::cout << index << ',' << lob::mm::to_string(kind) << ',' << market.exchange_timestamp_ns
              << ',' << config.strategy.session_end_ns << ',' << inventory << ',' << std::hexfloat
              << config.strategy.gamma << ',' << config.strategy.sigma_squared << ','
              << config.strategy.arrival_intensity_k << ',' << market.causal_signal << ','
              << config.strategy.signal_coefficient_price4 << ',' << std::defaultfloat
              << (quote.bid_price4.has_value() ? static_cast<std::int64_t>(*quote.bid_price4) : -1)
              << ','
              << (quote.ask_price4.has_value() ? static_cast<std::int64_t>(*quote.ask_price4) : -1)
              << ',' << std::hexfloat
              << quote.reservation_price4.value_or(std::numeric_limits<double>::quiet_NaN()) << ','
              << quote.total_spread_price4.value_or(std::numeric_limits<double>::quiet_NaN())
              << std::defaultfloat << '\n';
  }
  return std::cout ? 0 : 3;
}
