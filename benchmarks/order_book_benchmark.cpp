#include "lob/order_book.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

#ifndef LOB_COMPILER_FLAGS
#define LOB_COMPILER_FLAGS "unknown"
#endif

namespace {

using namespace lob;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kBenchmarkSeed = 12'648'430;

struct Workload {
  std::string name;
  std::vector<Command> setup;
  std::vector<Command> commands;
};

NewOrder resting(const OrderId id, const Side side, const Price price, const Quantity quantity) {
  return NewOrder{id, side, OrderType::Limit, TimeInForce::GTC, price, quantity, 0};
}

NewOrder market(const OrderId id, const Side side, const Quantity quantity) {
  return NewOrder{id, side, OrderType::Market, TimeInForce::IOC, std::nullopt, quantity, 0};
}

Workload make_add_cancel(const std::size_t count, std::mt19937_64 &rng) {
  Workload workload{"add_cancel", {}, {}};
  workload.commands.reserve(count);
  OrderId id = 1;
  while (workload.commands.size() < count) {
    workload.commands.emplace_back(resting(id, Side::Buy, 9'900, 50 + (rng() % 101)));
    if (workload.commands.size() < count) {
      workload.commands.emplace_back(CancelOrder{id});
    }
    ++id;
  }
  return workload;
}

Workload make_crossing(const std::size_t count, std::mt19937_64 &rng) {
  Workload workload{"crossing", {}, {}};
  workload.commands.reserve(count);
  OrderId id = 1;
  while (workload.commands.size() < count) {
    const Price base_price = 10'000 + static_cast<Price>(rng() % 11);
    std::vector<OrderId> makers;
    for (Price offset = 0; offset < 4 && workload.commands.size() < count; ++offset) {
      makers.push_back(id);
      workload.commands.emplace_back(resting(id++, Side::Sell, base_price + offset, 10));
    }
    if (workload.commands.size() < count) {
      workload.commands.emplace_back(market(id++, Side::Buy, 35));
    }
    if (workload.commands.size() < count && makers.size() == 4) {
      workload.commands.emplace_back(CancelOrder{makers.back()});
    }
  }
  return workload;
}

Workload make_mixed(const std::size_t count, std::mt19937_64 &rng) {
  Workload workload{"mixed", {}, {}};
  workload.commands.reserve(count);
  OrderId id = 1;
  std::uint64_t cycle = 0;
  const auto append = [&workload, count](Command command) {
    if (workload.commands.size() < count) {
      workload.commands.push_back(std::move(command));
    }
  };
  while (workload.commands.size() < count) {
    const Price shift = static_cast<Price>((cycle + (rng() % 7)) % 20);
    const OrderId b1 = id++;
    const OrderId b2 = id++;
    const OrderId b3 = id++;
    const OrderId b4 = id++;
    const OrderId a1 = id++;
    const OrderId a2 = id++;
    const OrderId a3 = id++;
    const OrderId a4 = id++;
    const OrderId a5 = id++;
    append(resting(b1, Side::Buy, 9'900 + shift, 100));
    append(resting(b2, Side::Buy, 9'901 + shift, 100));
    append(resting(b3, Side::Buy, 9'902 + shift, 100));
    append(resting(b4, Side::Buy, 9'903 + shift, 100));
    append(resting(a1, Side::Sell, 10'100 + shift, 100));
    append(resting(a2, Side::Sell, 10'101 + shift, 100));
    append(resting(a3, Side::Sell, 10'102 + shift, 100));
    append(resting(a4, Side::Sell, 10'103 + shift, 100));
    append(resting(a5, Side::Sell, 10'104 + shift, 100));
    append(ReduceOrder{b1, 1});
    append(CancelOrder{b2});
    append(ReduceOrder{a1, 1});
    append(CancelOrder{a2});
    append(ReduceOrder{a3, 1});
    append(market(id++, Side::Sell, 50));
    append(market(id++, Side::Buy, 60));
    append(NewOrder{id++, Side::Buy, OrderType::Limit, TimeInForce::IOC, 10'200, 20, 0});
    append(NewOrder{id++, Side::Sell, OrderType::Limit, TimeInForce::IOC, 9'800, 20, 0});
    append(ReplaceOrder{b4, 9'903 + shift, 110, 0});
    append(ReplaceOrder{a4, 10'105 + shift, 90, 0});
    ++cycle;
  }
  return workload;
}

Workload make_deep_book(const std::size_t count, std::mt19937_64 &rng) {
  Workload workload{"deep_book", {}, {}};
  constexpr std::size_t levels_per_side = 200;
  constexpr std::size_t orders_per_level = 50;
  std::vector<OrderId> live_ids;
  std::vector<std::pair<Side, Price>> slots;
  OrderId next_id = 1;
  for (std::size_t side_index = 0; side_index < 2; ++side_index) {
    const Side side = side_index == 0 ? Side::Buy : Side::Sell;
    for (std::size_t level = 0; level < levels_per_side; ++level) {
      const Price price = side == Side::Buy ? 9'000 - static_cast<Price>(level)
                                            : 11'000 + static_cast<Price>(level);
      for (std::size_t order = 0; order < orders_per_level; ++order) {
        live_ids.push_back(next_id);
        slots.emplace_back(side, price);
        workload.setup.emplace_back(resting(next_id++, side, price, 50 + (rng() % 101)));
      }
    }
  }
  workload.commands.reserve(count);
  std::size_t slot = 0;
  while (workload.commands.size() < count) {
    workload.commands.emplace_back(CancelOrder{live_ids[slot]});
    if (workload.commands.size() < count) {
      const auto [side, price] = slots[slot];
      live_ids[slot] = next_id;
      workload.commands.emplace_back(resting(next_id++, side, price, 100));
    }
    slot = (slot + 1) % live_ids.size();
  }
  return workload;
}

void apply_setup(OrderBook &book, const std::vector<Command> &commands) {
  for (const auto &command : commands) {
    (void)book.process(command);
  }
}

std::uint64_t fnv1a(std::string_view text, std::uint64_t value) {
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    value ^= byte;
    value *= prime;
  }
  return value;
}

struct Result {
  double commands_per_second{};
  std::uint64_t p50_ns{};
  std::uint64_t p95_ns{};
  std::uint64_t p99_ns{};
  std::uint64_t checksum{};
};

std::uint64_t percentile(const std::vector<std::uint64_t> &sorted, const std::size_t numerator) {
  const std::size_t index = ((sorted.size() * numerator) + 99) / 100 - 1;
  return sorted[index];
}

Result run(const Workload &workload) {
  const std::size_t warmup_count = std::min<std::size_t>(100'000, workload.commands.size());
  {
    OrderBook warmup;
    apply_setup(warmup, workload.setup);
    for (std::size_t index = 0; index < warmup_count; ++index) {
      (void)warmup.process(workload.commands[index]);
    }
  }

  OrderBook throughput_book;
  apply_setup(throughput_book, workload.setup);
  std::size_t event_count = 0;
  const auto throughput_start = Clock::now();
  for (const auto &command : workload.commands) {
    event_count += throughput_book.process(command).size();
  }
  const auto throughput_end = Clock::now();
  const double seconds = std::chrono::duration<double>(throughput_end - throughput_start).count();

  OrderBook latency_book;
  apply_setup(latency_book, workload.setup);
  std::vector<std::uint64_t> latencies;
  latencies.reserve(workload.commands.size());
  for (const auto &command : workload.commands) {
    const auto start = Clock::now();
    (void)latency_book.process(command);
    const auto end = Clock::now();
    latencies.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
  }
  std::sort(latencies.begin(), latencies.end());

  std::uint64_t checksum =
      fnv1a(throughput_book.canonical_snapshot(), 1'469'598'103'934'665'603ULL);
  checksum ^= static_cast<std::uint64_t>(event_count);
  checksum *= 1'099'511'628'211ULL;
  std::string error;
  if (!throughput_book.check_invariants(&error) ||
      throughput_book.canonical_snapshot() != latency_book.canonical_snapshot()) {
    std::cerr << "benchmark validation failed for " << workload.name << ": " << error << '\n';
    std::exit(2);
  }
  return Result{static_cast<double>(workload.commands.size()) / seconds, percentile(latencies, 50),
                percentile(latencies, 95), percentile(latencies, 99), checksum};
}

std::string cpu_brand() {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  const auto maximum = static_cast<unsigned int>(__get_cpuid_max(0x80000000U, nullptr));
  if (maximum >= 0x80000004U) {
    std::string brand(48, '\0');
    for (unsigned int leaf = 0; leaf < 3; ++leaf) {
      unsigned int values[4]{};
      __cpuid(0x80000002U + leaf, values[0], values[1], values[2], values[3]);
      for (unsigned int index = 0; index < 4; ++index) {
        const auto value = values[index];
        for (unsigned int byte = 0; byte < 4; ++byte) {
          brand[leaf * 16 + index * 4 + byte] = static_cast<char>((value >> (byte * 8U)) & 0xFFU);
        }
      }
    }
    while (!brand.empty() && (brand.back() == '\0' || brand.back() == ' ')) {
      brand.pop_back();
    }
    const auto first = brand.find_first_not_of(' ');
    return first == std::string::npos ? "unknown" : brand.substr(first);
  }
#endif
  return "unknown";
}

bool hypervisor_present() {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;
  if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0) {
    return (ecx & (1U << 31U)) != 0;
  }
#endif
  return false;
}

std::string compiler() {
#if defined(__clang__)
  return std::string{"Clang "} + __clang_version__;
#elif defined(__GNUC__)
  return std::string{"GCC "} + __VERSION__;
#elif defined(_MSC_VER)
  return std::string{"MSVC "} + std::to_string(_MSC_VER);
#else
  return "unknown";
#endif
}

std::string operating_system() {
#if defined(_WIN32)
  return "Windows";
#elif defined(__linux__)
  return "Linux";
#elif defined(__APPLE__)
  return "macOS";
#else
  return "unknown";
#endif
}

} // namespace

int main(int argc, char **argv) {
  std::size_t command_count = 1'000'000;
  if (argc == 2 && std::string_view{argv[1]} == "--smoke") {
    command_count = 10'000;
  } else if (argc == 3 && std::string_view{argv[1]} == "--commands") {
    command_count = static_cast<std::size_t>(std::stoull(argv[2]));
  }
  if (command_count == 0) {
    std::cerr << "command count must be positive\n";
    return 1;
  }

  std::cout << "lobforge_benchmark_v1\n"
            << "clock=std::chrono::steady_clock per_command_latency=single_thread_sequential_ns\n"
            << "rng=mt19937_64 seed=" << kBenchmarkSeed
            << " generation_outside_timed_region=true warmup_commands="
            << std::min<std::size_t>(100'000, command_count) << '\n'
            << "compiler=" << compiler() << '\n'
            << "optimization_flags=" << LOB_COMPILER_FLAGS << '\n'
            << "cpu_model=" << cpu_brand() << '\n'
            << "logical_cores=" << std::thread::hardware_concurrency() << '\n'
            << "operating_system=" << operating_system() << '\n'
            << "virtualized=" << (hypervisor_present() ? "yes" : "no_or_undetected") << '\n'
            << "workload commands_per_second p50_ns p95_ns p99_ns checksum\n";

  std::vector<Workload> workloads;
  std::mt19937_64 rng(kBenchmarkSeed);
  workloads.push_back(make_add_cancel(command_count, rng));
  workloads.push_back(make_crossing(command_count, rng));
  workloads.push_back(make_mixed(command_count, rng));
  workloads.push_back(make_deep_book(command_count, rng));
  for (const auto &workload : workloads) {
    const Result result = run(workload);
    std::cout << workload.name << ' ' << static_cast<std::uint64_t>(result.commands_per_second)
              << ' ' << result.p50_ns << ' ' << result.p95_ns << ' ' << result.p99_ns << ' '
              << result.checksum << '\n';
  }
  return 0;
}
