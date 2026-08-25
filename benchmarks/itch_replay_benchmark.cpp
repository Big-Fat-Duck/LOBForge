#include "lob/itch/decoder.hpp"
#include "lob/replay/factual_book.hpp"
#include "lob/replay/replayer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
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

using Clock = std::chrono::steady_clock;

void append_be(std::vector<std::byte> &bytes, std::uint64_t value, const std::size_t width) {
  for (std::size_t index = width; index != 0; --index) {
    const auto shift = static_cast<unsigned int>((index - 1) * 8);
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_ascii(std::vector<std::byte> &bytes, const std::string_view text,
                  const std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    bytes.push_back(static_cast<std::byte>(index < text.size() ? text[index] : ' '));
  }
}

std::vector<std::byte> common(const char type, const std::uint16_t locate,
                              const std::uint64_t timestamp) {
  std::vector<std::byte> bytes;
  bytes.reserve(50);
  bytes.push_back(static_cast<std::byte>(type));
  append_be(bytes, locate, 2);
  append_be(bytes, 1, 2);
  append_be(bytes, timestamp, 6);
  return bytes;
}

std::vector<std::byte> directory(const std::uint16_t locate, const std::string_view symbol) {
  auto bytes = common('R', locate, 1);
  append_ascii(bytes, symbol, 8);
  append_ascii(bytes, "Q", 1);
  append_ascii(bytes, "N", 1);
  append_be(bytes, 100, 4);
  append_ascii(bytes, "NC  PNN1N", 9);
  append_be(bytes, 1, 4);
  append_ascii(bytes, "N", 1);
  return bytes;
}

std::vector<std::byte> add(const std::uint16_t locate, const std::string_view symbol,
                           const std::uint64_t reference, const char side,
                           const std::uint32_t shares, const std::uint32_t price,
                           const std::uint64_t timestamp) {
  auto bytes = common('A', locate, timestamp);
  append_be(bytes, reference, 8);
  append_ascii(bytes, std::string_view{&side, 1}, 1);
  append_be(bytes, shares, 4);
  append_ascii(bytes, symbol, 8);
  append_be(bytes, price, 4);
  return bytes;
}

std::vector<std::byte> execute(const std::uint16_t locate, const std::uint64_t reference,
                               const std::uint32_t shares, const std::uint64_t match,
                               const std::uint64_t timestamp) {
  auto bytes = common('E', locate, timestamp);
  append_be(bytes, reference, 8);
  append_be(bytes, shares, 4);
  append_be(bytes, match, 8);
  return bytes;
}

std::vector<std::byte> cancel(const std::uint16_t locate, const std::uint64_t reference,
                              const std::uint32_t shares, const std::uint64_t timestamp) {
  auto bytes = common('X', locate, timestamp);
  append_be(bytes, reference, 8);
  append_be(bytes, shares, 4);
  return bytes;
}

std::vector<std::byte> replace(const std::uint16_t locate, const std::uint64_t old_reference,
                               const std::uint64_t new_reference, const std::uint32_t shares,
                               const std::uint32_t price, const std::uint64_t timestamp) {
  auto bytes = common('U', locate, timestamp);
  append_be(bytes, old_reference, 8);
  append_be(bytes, new_reference, 8);
  append_be(bytes, shares, 4);
  append_be(bytes, price, 4);
  return bytes;
}

std::vector<std::byte> erase(const std::uint16_t locate, const std::uint64_t reference,
                             const std::uint64_t timestamp) {
  auto bytes = common('D', locate, timestamp);
  append_be(bytes, reference, 8);
  return bytes;
}

std::vector<std::byte> from_hex(const std::string_view hex) {
  const auto nibble = [](const char value) {
    if (value <= '9') {
      return static_cast<unsigned int>(value - '0');
    }
    return 10U + static_cast<unsigned int>(value - 'A');
  };
  std::vector<std::byte> result;
  result.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    result.push_back(static_cast<std::byte>((nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
  }
  return result;
}

const std::vector<std::vector<std::byte>> &decoder_payloads() {
  static const std::vector<std::vector<std::byte>> payloads = [] {
    constexpr std::string_view fixture_hex =
        "53000000020102030405064F "
        "52000100020102030405064141504C20202020464E000000644E432020504E5A314E000000014E "
        "48000100020102030405064141504C20202020542020202020 "
        "59000100020102030405064141504C2020202030 "
        "4C00010002010203040506414243444141504C20202020594E41 "
        "56000000020102030405060000000005F5E100000000000BEBC2000000000011E1A300 "
        "570000000201020304050631 "
        "4B00000002010203040506544553542020202000008598410012D644 "
        "4A000100020102030405064141504C20202020000F42400010C8E0000DBBA000000002 "
        "68000100020102030405064141504C202020205148 "
        "4100010002010203040506010203040506070842000000644141504C202020200012D644 "
        "4600010002010203040506111213141516171853000000C84141504C202020200012D6A841424344 "
        "45000100020102030405060102030405060708000000192122232425262728 "
        "430001000201020304050611121314151617180000001E31323334353637384E0012D676 "
        "580001000201020304050601020304050607080000000A "
        "44000100020102030405061112131415161718 "
        "550001000201020304050601020304050607084142434445464748000000500012D5E0 "
        "5000010002010203040506000000000000000042000000324141504C202020200012D6445152535455565758 "
        "510001000201020304050600000000000003E84141504C202020200012D64461626364656667684F "
        "42000100020102030405065152535455565758 "
        "490001000201020304050600000000000003E800000000000000C8424141504C2020202000124F800012769000"
        "129DA04F4C "
        "4E000100020102030405064141504C2020202042 "
        "4F000100020102030405064141504C2020202059000F4240001E84800016E36000000000075BCD1500155CC000"
        "186A00";
    std::vector<std::vector<std::byte>> result;
    std::size_t start = 0;
    while (start < fixture_hex.size()) {
      const std::size_t end = fixture_hex.find(' ', start);
      const auto piece = fixture_hex.substr(
          start, end == std::string_view::npos ? fixture_hex.size() - start : end - start);
      result.push_back(from_hex(piece));
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return result;
  }();
  return payloads;
}

struct RecordWorkload {
  std::string name;
  std::vector<std::vector<std::byte>> setup;
  std::vector<std::vector<std::byte>> records;
};

RecordWorkload make_reconstruction(const std::size_t count) {
  RecordWorkload workload{"decoder_apply", {directory(1, "AAPL")}, {}};
  workload.records.reserve(count);
  std::uint64_t next_reference = 1;
  std::uint64_t next_match = 1;
  std::uint64_t timestamp = 2;
  while (workload.records.size() < count) {
    const std::uint64_t old_reference = next_reference++;
    const std::uint64_t new_reference = next_reference++;
    const auto append = [&](std::vector<std::byte> record) {
      if (workload.records.size() < count) {
        workload.records.push_back(std::move(record));
      }
    };
    append(add(1, "AAPL", old_reference, 'B', 100, 1'000'000, timestamp++));
    append(execute(1, old_reference, 20, next_match++, timestamp++));
    append(cancel(1, old_reference, 10, timestamp++));
    append(replace(1, old_reference, new_reference, 80, 1'000'100, timestamp++));
    append(erase(1, new_reference, timestamp++));
  }
  return workload;
}

RecordWorkload make_deep(const std::size_t count) {
  RecordWorkload workload{"deep_multi_symbol", {}, {}};
  constexpr std::size_t symbol_count = 64;
  constexpr std::size_t orders_per_symbol = 50;
  std::vector<std::uint64_t> live;
  std::vector<std::uint16_t> locates;
  std::vector<std::string> symbols;
  std::vector<char> sides;
  std::vector<std::uint32_t> prices;
  std::uint64_t next_reference = 1;
  for (std::size_t symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
    std::string symbol = "S" + std::to_string(symbol_index);
    const auto locate = static_cast<std::uint16_t>(symbol_index + 1);
    workload.setup.push_back(directory(locate, symbol));
    for (std::size_t order_index = 0; order_index < orders_per_symbol; ++order_index) {
      const char side = (order_index & 1U) == 0 ? 'B' : 'S';
      const auto price =
          static_cast<std::uint32_t>(side == 'B' ? 900'000 - order_index : 1'100'000 + order_index);
      workload.setup.push_back(
          add(locate, symbol, next_reference, side, 100, price, next_reference + 100));
      live.push_back(next_reference++);
      locates.push_back(locate);
      symbols.push_back(symbol);
      sides.push_back(side);
      prices.push_back(price);
    }
  }
  workload.records.reserve(count);
  std::size_t slot = 0;
  std::uint64_t timestamp = next_reference + 100;
  while (workload.records.size() < count) {
    workload.records.push_back(erase(locates[slot], live[slot], timestamp++));
    if (workload.records.size() < count) {
      live[slot] = next_reference;
      workload.records.push_back(add(locates[slot], symbols[slot], next_reference++, sides[slot],
                                     100, prices[slot], timestamp++));
    }
    slot = (slot + 1) % live.size();
  }
  return workload;
}

void apply_records(lob::replay::FactualState &state,
                   const std::vector<std::vector<std::byte>> &records) {
  for (const auto &payload : records) {
    const auto decoded = lob::itch::decode_message(payload);
    if (!decoded.ok() || !state.apply(*decoded.message).ok()) {
      std::cerr << "benchmark setup is invalid\n";
      std::exit(2);
    }
  }
}

std::uint64_t percentile(const std::vector<std::uint64_t> &sorted, const std::size_t value) {
  return sorted[((sorted.size() * value) + 99) / 100 - 1];
}

struct Result {
  double throughput{};
  std::uint64_t p50{};
  std::uint64_t p95{};
  std::uint64_t p99{};
  std::uint64_t checksum{};
};

Result run_decoder(const std::size_t count) {
  const auto &payloads = decoder_payloads();
  std::uint64_t checksum = 0;
  for (std::size_t index = 0; index < std::min<std::size_t>(100'000, count); ++index) {
    const auto result = lob::itch::decode_message(payloads[index % payloads.size()]);
    checksum += static_cast<unsigned char>(lob::itch::message_type(*result.message));
  }
  checksum = 0;
  const auto start = Clock::now();
  for (std::size_t index = 0; index < count; ++index) {
    const auto result = lob::itch::decode_message(payloads[index % payloads.size()]);
    checksum += static_cast<unsigned char>(lob::itch::message_type(*result.message));
    checksum += lob::itch::common_header(*result.message).timestamp;
  }
  const auto end = Clock::now();
  std::vector<std::uint64_t> latencies;
  latencies.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto before = Clock::now();
    (void)lob::itch::decode_message(payloads[index % payloads.size()]);
    const auto after = Clock::now();
    latencies.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
  }
  std::sort(latencies.begin(), latencies.end());
  return {static_cast<double>(count) / std::chrono::duration<double>(end - start).count(),
          percentile(latencies, 50), percentile(latencies, 95), percentile(latencies, 99),
          checksum};
}

Result run_apply(const RecordWorkload &workload) {
  const std::size_t warmup_count = std::min<std::size_t>(100'000, workload.records.size());
  {
    lob::replay::FactualState warmup;
    apply_records(warmup, workload.setup);
    for (std::size_t i = 0; i < warmup_count; ++i) {
      const auto decoded = lob::itch::decode_message(workload.records[i]);
      if (!decoded.ok()) {
        throw std::runtime_error("benchmark warmup decoder failure");
      }
      const auto applied = warmup.apply(*decoded.message);
      if (!applied.ok()) {
        throw std::runtime_error("benchmark warmup apply failure");
      }
    }
  }
  lob::replay::FactualState state;
  apply_records(state, workload.setup);
  const auto start = Clock::now();
  apply_records(state, workload.records);
  const auto end = Clock::now();

  lob::replay::FactualState latency_state;
  apply_records(latency_state, workload.setup);
  std::vector<std::uint64_t> latencies;
  latencies.reserve(workload.records.size());
  for (const auto &payload : workload.records) {
    const auto before = Clock::now();
    const auto decoded = lob::itch::decode_message(payload);
    const auto applied = latency_state.apply(*decoded.message);
    const auto after = Clock::now();
    if (!applied.ok()) {
      std::cerr << "latency replay is invalid\n";
      std::exit(2);
    }
    latencies.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
  }
  std::sort(latencies.begin(), latencies.end());
  if (state.digest() != latency_state.digest()) {
    std::cerr << "throughput and latency replay states differ\n";
    std::exit(2);
  }
  return {static_cast<double>(workload.records.size()) /
              std::chrono::duration<double>(end - start).count(),
          percentile(latencies, 50), percentile(latencies, 95), percentile(latencies, 99),
          state.digest()};
}

Result run_error_path(const std::size_t count) {
  const auto good_payload = directory(1, "AAPL");
  const auto bad_payload = from_hex("5A");
  std::vector<std::byte> file;
  file.reserve(count * 41);
  for (std::size_t index = 0; index < count; ++index) {
    const auto &payload = (index % 1000) == 999 ? bad_payload : good_payload;
    append_be(file, payload.size(), 2);
    file.insert(file.end(), payload.begin(), payload.end());
  }
  lob::replay::Replayer replayer;
  const auto start = Clock::now();
  const bool success = replayer.run(file, lob::replay::ReplayMode::Permissive);
  const auto end = Clock::now();
  if (!success || replayer.statistics().records_seen != count ||
      replayer.statistics().records_failed != count / 1000) {
    std::cerr << "permissive benchmark validation failed\n";
    std::exit(2);
  }
  return {static_cast<double>(count) / std::chrono::duration<double>(end - start).count(), 0, 0, 0,
          replayer.digest()};
}

std::string compiler() {
#if defined(__clang__)
  return std::string{"Clang "} + __clang_version__;
#elif defined(__GNUC__)
  return std::string{"GCC "} + __VERSION__;
#else
  return "unknown";
#endif
}

std::string cpu_brand() {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  if (static_cast<unsigned int>(__get_cpuid_max(0x80000000U, nullptr)) >= 0x80000004U) {
    std::string brand(48, '\0');
    for (unsigned int leaf = 0; leaf < 3; ++leaf) {
      unsigned int values[4]{};
      __cpuid(0x80000002U + leaf, values[0], values[1], values[2], values[3]);
      for (unsigned int index = 0; index < 4; ++index) {
        for (unsigned int byte = 0; byte < 4; ++byte) {
          brand[leaf * 16 + index * 4 + byte] =
              static_cast<char>((values[index] >> (byte * 8U)) & 0xFFU);
        }
      }
    }
    while (!brand.empty() && (brand.back() == '\0' || brand.back() == ' ')) {
      brand.pop_back();
    }
    return brand.substr(brand.find_first_not_of(' '));
  }
#endif
  return "unknown";
}

} // namespace

int main(int argc, char **argv) {
  std::size_t count = 1'000'000;
  if (argc == 2 && std::string_view{argv[1]} == "--smoke") {
    count = 10'000;
  }
  std::cout << "lobforge_itch_benchmark_v1\n"
            << "compiler=" << compiler() << '\n'
            << "optimization_flags=" << LOB_COMPILER_FLAGS << '\n'
            << "cpu_model=" << cpu_brand() << '\n'
            << "logical_cores=" << std::thread::hardware_concurrency() << '\n'
#if defined(_WIN32)
            << "operating_system=Windows\n"
#else
            << "operating_system=Linux\n"
#endif
            << "clock=std::chrono::steady_clock input=preloaded_memory warmup_records="
            << std::min<std::size_t>(100'000, count) << " sample_records=" << count << '\n'
            << "workload records_per_second p50_ns p95_ns p99_ns checksum distribution\n";

  const Result decoder = run_decoder(count);
  std::cout << "decoder_only " << static_cast<std::uint64_t>(decoder.throughput) << ' '
            << decoder.p50 << ' ' << decoder.p95 << ' ' << decoder.p99 << ' ' << decoder.checksum
            << " uniform_23_types\n";
  const auto apply_workload = make_reconstruction(count);
  const Result apply = run_apply(apply_workload);
  std::cout << "decoder_apply " << static_cast<std::uint64_t>(apply.throughput) << ' ' << apply.p50
            << ' ' << apply.p95 << ' ' << apply.p99 << ' ' << apply.checksum
            << " add20_execute20_cancel20_replace20_delete20\n";
  const auto deep_workload = make_deep(count);
  const Result deep = run_apply(deep_workload);
  std::cout << "deep_multi_symbol " << static_cast<std::uint64_t>(deep.throughput) << ' '
            << deep.p50 << ' ' << deep.p95 << ' ' << deep.p99 << ' ' << deep.checksum
            << " symbols64_levels3200_delete50_add50\n";
  const Result errors = run_error_path(count);
  std::cout << "permissive_error " << static_cast<std::uint64_t>(errors.throughput) << ' '
            << errors.p50 << ' ' << errors.p95 << ' ' << errors.p99 << ' ' << errors.checksum
            << " invalid_rate_0.1_percent\n";
  return 0;
}
