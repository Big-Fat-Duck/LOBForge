#include "lob/mm/simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::uint64_t events{1'000'000};
  std::uint64_t runs{5};
  std::uint64_t warmup{100'000};
};

std::optional<std::uint64_t> integer(const std::string_view text) {
  std::uint64_t value = 0;
  if (text.empty()) {
    return std::nullopt;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    value = value * 10U + digit;
  }
  return value == 0 ? std::nullopt : std::optional<std::uint64_t>{value};
}

std::optional<Options> parse_options(const int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    if (index + 1 >= argc) {
      return std::nullopt;
    }
    const std::string_view key{argv[index++]};
    const auto value = integer(argv[index]);
    if (!value.has_value()) {
      return std::nullopt;
    }
    if (key == "--events") {
      options.events = *value;
    } else if (key == "--runs") {
      options.runs = *value;
    } else if (key == "--warmup") {
      options.warmup = *value;
    } else {
      return std::nullopt;
    }
  }
  return options;
}

template <std::size_t Size> lob::itch::FixedAscii<Size> fixed(const std::string_view value) {
  lob::itch::FixedAscii<Size> result;
  result.raw.fill(' ');
  std::copy_n(value.begin(), std::min(value.size(), Size), result.raw.begin());
  return result;
}

lob::replay::FactualState initialized_state() {
  lob::replay::FactualState state;
  const auto apply = [&](const lob::itch::Message &message) {
    if (!state.apply(message).ok()) {
      throw std::runtime_error("benchmark factual initialization failed");
    }
  };
  const auto header = [](const lob::itch::StockLocate locate, const std::uint64_t timestamp) {
    return lob::itch::CommonHeader{locate, 1, timestamp};
  };
  apply(lob::itch::SystemEvent{header(0, 1), 'O'});
  apply(lob::itch::SystemEvent{header(0, 2), 'S'});
  lob::itch::StockDirectory directory{};
  directory.header = header(1, 3);
  directory.stock = fixed<8>("BENCH");
  directory.market_category = 'Q';
  directory.financial_status = 'N';
  directory.round_lot_size = 100;
  directory.round_lots_only = 'N';
  directory.issue_classification = 'C';
  directory.authenticity = 'P';
  directory.short_sale_threshold_indicator = 'N';
  directory.ipo_flag = 'N';
  directory.luld_reference_price_tier = '1';
  directory.etp_flag = 'N';
  directory.etp_leverage_factor = 1;
  directory.inverse_indicator = 'N';
  apply(directory);
  apply(lob::itch::SystemEvent{header(0, 4), 'Q'});
  apply(lob::itch::StockTradingAction{header(1, 5), fixed<8>("BENCH"), 'T', ' ', fixed<4>("")});
  apply(lob::itch::AddOrder{header(1, 6), 1, lob::itch::FeedSide::Buy, 100, fixed<8>("BENCH"),
                            10'000});
  apply(lob::itch::AddOrder{header(1, 7), 2, lob::itch::FeedSide::Sell, 100, fixed<8>("BENCH"),
                            10'002});
  return state;
}

std::uint64_t peak_rss_bytes() {
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key == "VmHWM:") {
      std::uint64_t kibibytes = 0;
      std::string unit;
      status >> kibibytes >> unit;
      return kibibytes * 1'024U;
    }
    std::string ignored;
    std::getline(status, ignored);
  }
  return 0;
}

struct RunResult {
  double events_per_second{};
  double p99_microseconds{};
};

RunResult run(const std::uint64_t events) {
  auto state = initialized_state();
  lob::mm::SimulationConfig config;
  config.automatic_strategy = true;
  config.strategy_kind = lob::mm::StrategyKind::SymmetricQuote;
  config.latency = {};
  config.quote.tick_size_price4 = 1;
  config.quote.minimum_rest_ns = 0;
  config.strategy.symmetric_half_spread_price4 = 1.0;
  config.strategy.session_end_ns = std::numeric_limits<std::uint64_t>::max();
  config.risk.stop_new_quotes_before_close_ns = 0;
  lob::mm::ShadowSimulator simulator(config);
  std::vector<double> latencies;
  latencies.reserve(static_cast<std::size_t>(events));
  const auto total_start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 1; index <= events; ++index) {
    const auto timestamp = 1'000ULL + index;
    const lob::itch::Message message{lob::itch::RegShoRestriction{
        lob::itch::CommonHeader{1, 1, timestamp}, fixed<8>("BENCH"), '0'}};
    const auto start = std::chrono::steady_clock::now();
    simulator.before_market(timestamp, index, state);
    const auto applied = state.apply(message);
    if (!applied.ok()) {
      throw std::runtime_error("benchmark factual apply failed");
    }
    simulator.after_market(message, index, state);
    const auto end = std::chrono::steady_clock::now();
    latencies.push_back(std::chrono::duration<double, std::micro>(end - start).count());
  }
  simulator.finish(1'001ULL + events, state);
  const auto total_end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(total_end - total_start).count();
  const auto p99_position =
      latencies.begin() + static_cast<std::ptrdiff_t>((latencies.size() - 1U) * 99U / 100U);
  std::nth_element(latencies.begin(), p99_position, latencies.end());
  return {static_cast<double>(events) / seconds, *p99_position};
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

} // namespace

int main(int argc, char **argv) {
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: mm_benchmark [--events N] [--runs N] [--warmup N]\n";
    return 2;
  }
  (void)run(options->warmup);
  std::vector<RunResult> results;
  for (std::uint64_t index = 0; index < options->runs; ++index) {
    results.push_back(run(options->events));
  }
  std::vector<double> throughput;
  std::vector<double> p99;
  for (const auto &result : results) {
    throughput.push_back(result.events_per_second);
    p99.push_back(result.p99_microseconds);
  }
  const auto throughput_median = median(throughput);
  const auto p99_median = median(p99);
  const auto rss = peak_rss_bytes();
#if defined(__clang__)
  constexpr std::string_view compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
  constexpr std::string_view compiler = "GCC " __VERSION__;
#else
  constexpr std::string_view compiler = "unknown";
#endif
  std::cout << std::fixed << std::setprecision(3)
            << "{\"schema\":\"lobforge.mm_benchmark\",\"version\":1,\"events\":" << options->events
            << ",\"warmup\":" << options->warmup << ",\"runs\":" << options->runs
            << ",\"compiler\":\"" << compiler << "\",\"optimization_flags\":\""
            << LOB_COMPILER_FLAGS << "\""
            << ",\"events_per_second_runs\":[";
  for (std::size_t index = 0; index < throughput.size(); ++index) {
    std::cout << (index == 0 ? "" : ",") << throughput[index];
  }
  std::cout << "],\"p99_microseconds_runs\":[";
  for (std::size_t index = 0; index < p99.size(); ++index) {
    std::cout << (index == 0 ? "" : ",") << p99[index];
  }
  std::cout << "],\"median_events_per_second\":" << throughput_median
            << ",\"median_p99_microseconds\":" << p99_median << ",\"peak_rss_bytes\":" << rss
            << ",\"throughput_pass\":" << (throughput_median >= 500'000.0 ? "true" : "false")
            << ",\"p99_pass\":" << (p99_median < 20.0 ? "true" : "false")
            << ",\"rss_pass\":" << (rss != 0 && rss <= 1'610'612'736ULL ? "true" : "false")
            << "}\n";
  return 0;
}
