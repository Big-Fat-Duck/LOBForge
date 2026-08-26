#include "lob/mm/protocol.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lob::mm {
namespace {

constexpr std::array<std::uint32_t, 64> sha_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

std::uint32_t rotate_right(const std::uint32_t value, const unsigned count) noexcept {
  return (value >> count) | (value << (32U - count));
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string remove_comment(const std::string &line) {
  bool quoted = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char value = line[index];
    if (quoted && value == '\\' && !escaped) {
      escaped = true;
      continue;
    }
    if (value == '"' && !escaped) {
      quoted = !quoted;
    } else if (value == '#' && !quoted) {
      return line.substr(0, index);
    }
    escaped = false;
  }
  if (quoted) {
    throw std::runtime_error("unterminated quoted protocol value");
  }
  return line;
}

std::string normalize_value(const std::string &value) {
  std::string output;
  bool quoted = false;
  bool escaped = false;
  for (const char current : trim(value)) {
    if (quoted && current == '\\' && !escaped) {
      escaped = true;
      output.push_back(current);
      continue;
    }
    if (current == '"' && !escaped) {
      quoted = !quoted;
      output.push_back(current);
    } else if (!quoted && (current == ' ' || current == '\t' || current == '\r')) {
      continue;
    } else {
      output.push_back(current);
    }
    escaped = false;
  }
  return output;
}

std::string string_value(const std::map<std::string, std::string> &values, const std::string &key) {
  const auto position = values.find(key);
  if (position == values.end()) {
    throw std::runtime_error("missing protocol key: " + key);
  }
  const auto &value = position->second;
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    throw std::runtime_error("protocol key is not a quoted string: " + key);
  }
  return value.substr(1, value.size() - 2);
}

template <typename Integer>
Integer integer_value(const std::map<std::string, std::string> &values, const std::string &key) {
  const auto position = values.find(key);
  if (position == values.end()) {
    throw std::runtime_error("missing protocol key: " + key);
  }
  Integer output{};
  const auto first = position->second.data();
  const auto last = first + position->second.size();
  const auto result = std::from_chars(first, last, output);
  if (result.ec != std::errc{} || result.ptr != last) {
    throw std::runtime_error("protocol key is not an integer: " + key);
  }
  return output;
}

double double_value(const std::map<std::string, std::string> &values, const std::string &key) {
  const auto position = values.find(key);
  if (position == values.end()) {
    throw std::runtime_error("missing protocol key: " + key);
  }
  std::size_t consumed = 0;
  const double output = std::stod(position->second, &consumed);
  if (consumed != position->second.size() || !std::isfinite(output)) {
    throw std::runtime_error("protocol key is not a finite number: " + key);
  }
  return output;
}

bool bool_value(const std::map<std::string, std::string> &values, const std::string &key) {
  const auto position = values.find(key);
  if (position == values.end()) {
    throw std::runtime_error("missing protocol key: " + key);
  }
  if (position->second == "true") {
    return true;
  }
  if (position->second == "false") {
    return false;
  }
  throw std::runtime_error("protocol key is not a boolean: " + key);
}

std::vector<TimestampNs> integer_array(const std::map<std::string, std::string> &values,
                                       const std::string &key) {
  const auto position = values.find(key);
  if (position == values.end()) {
    throw std::runtime_error("missing protocol key: " + key);
  }
  const auto &raw = position->second;
  if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') {
    throw std::runtime_error("protocol key is not an array: " + key);
  }
  std::vector<TimestampNs> result;
  std::size_t begin = 1;
  while (begin < raw.size() - 1) {
    const auto comma = raw.find(',', begin);
    const auto end = comma == std::string::npos ? raw.size() - 1 : comma;
    const auto token = raw.substr(begin, end - begin);
    TimestampNs value{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
      throw std::runtime_error("protocol array contains a non-integer: " + key);
    }
    result.push_back(value);
    begin = end + 1;
  }
  if (result.empty()) {
    throw std::runtime_error("protocol array must not be empty: " + key);
  }
  return result;
}

void require_metadata(const std::map<std::string, std::string> &values) {
  static const std::array required{"protocol.schema_version",
                                   "latency.unit",
                                   "queue.primary",
                                   "queue.sensitivities",
                                   "calibration.fit_scope",
                                   "calibration.validation_refit",
                                   "calibration.test_refit",
                                   "markout.horizons_ns",
                                   "terminal.rule",
                                   "split.rule",
                                   "split.train_fraction",
                                   "split.validation_fraction",
                                   "split.test_fraction",
                                   "purge.rule",
                                   "purge.maximum_horizon_ns",
                                   "schemas.shadow_order",
                                   "schemas.shadow_fill",
                                   "schemas.inventory_event",
                                   "schemas.mm_summary",
                                   "ordering.key",
                                   "ordering.timestamp_tie_rule",
                                   "output.semantic_digest_basis",
                                   "evidence.synthetic_only_language"};
  for (const auto *key : required) {
    if (!values.contains(key)) {
      throw std::runtime_error(std::string("missing protocol key: ") + key);
    }
  }
}

} // namespace

std::string sha256_hex(const std::string_view bytes) {
  std::vector<std::uint8_t> padded(bytes.begin(), bytes.end());
  const auto bit_length = static_cast<std::uint64_t>(padded.size()) * 8ULL;
  padded.push_back(0x80U);
  while (padded.size() % 64 != 56) {
    padded.push_back(0U);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<std::uint8_t>(bit_length >> static_cast<unsigned>(shift)));
  }
  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto byte = offset + index * 4;
      words[index] = static_cast<std::uint32_t>(padded[byte]) << 24U |
                     static_cast<std::uint32_t>(padded[byte + 1]) << 16U |
                     static_cast<std::uint32_t>(padded[byte + 2]) << 8U |
                     static_cast<std::uint32_t>(padded[byte + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = rotate_right(words[index - 15], 7U) ^ rotate_right(words[index - 15], 18U) ^
                      (words[index - 15] >> 3U);
      const auto s1 = rotate_right(words[index - 2], 17U) ^ rotate_right(words[index - 2], 19U) ^
                      (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
      const auto choice = (e & f) ^ (~e & g);
      const auto temporary1 = h + sum1 + choice + sha_constants[index] + words[index];
      const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::hex << std::setfill('0');
  for (const auto value : hash) {
    output << std::setw(8) << value;
  }
  return output.str();
}

FrozenProtocol load_protocol(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open protocol file");
  }
  FrozenProtocol result;
  std::string section;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim(remove_comment(line));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      if (section.empty()) {
        throw std::runtime_error("empty protocol section at line " + std::to_string(line_number));
      }
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      throw std::runtime_error("invalid protocol assignment at line " +
                               std::to_string(line_number));
    }
    const auto key = trim(line.substr(0, equals));
    const auto value = normalize_value(line.substr(equals + 1));
    const auto full_key = section.empty() ? key : section + "." + key;
    if (key.empty() || value.empty() || !result.parsed_values.emplace(full_key, value).second) {
      throw std::runtime_error("invalid or duplicate protocol key at line " +
                               std::to_string(line_number));
    }
  }
  require_metadata(result.parsed_values);
  std::ostringstream canonical;
  canonical.imbue(std::locale::classic());
  for (const auto &[key, value] : result.parsed_values) {
    canonical << key << '=' << value << '\n';
  }
  result.canonical = canonical.str();
  result.sha256 = sha256_hex(result.canonical);

  auto &config = result.simulation;
  config.latency.market_data_ns =
      integer_value<TimestampNs>(result.parsed_values, "latency.market_data_ns");
  config.latency.strategy_compute_ns =
      integer_value<TimestampNs>(result.parsed_values, "latency.strategy_compute_ns");
  config.latency.order_entry_ns =
      integer_value<TimestampNs>(result.parsed_values, "latency.order_entry_ns");
  config.latency.cancel_ns = integer_value<TimestampNs>(result.parsed_values, "latency.cancel_ns");
  config.latency.replace_ns =
      integer_value<TimestampNs>(result.parsed_values, "latency.replace_ns");
  const auto queue_model = parse_queue_model(string_value(result.parsed_values, "queue.primary"));
  const auto strategy_kind =
      parse_strategy_kind(string_value(result.parsed_values, "strategy.primary"));
  if (!queue_model.has_value() || !strategy_kind.has_value()) {
    throw std::runtime_error("unknown queue model or strategy kind");
  }
  config.queue_model = *queue_model;
  config.strategy_kind = *strategy_kind;
  config.automatic_strategy = bool_value(result.parsed_values, "strategy.automatic");
  config.fees.maker_fee_nanos_per_share =
      integer_value<MoneyNanos>(result.parsed_values, "fees.maker_fee_nanos_per_share");
  config.fees.maker_rebate_nanos_per_share =
      integer_value<MoneyNanos>(result.parsed_values, "fees.maker_rebate_nanos_per_share");
  config.fees.liquidation_fee_nanos_per_share =
      integer_value<MoneyNanos>(result.parsed_values, "fees.liquidation_fee_nanos_per_share");
  config.quote.quantity = integer_value<std::uint64_t>(result.parsed_values, "quotes.quantity");
  config.quote.minimum_rest_ns =
      integer_value<TimestampNs>(result.parsed_values, "quotes.minimum_rest_ns");
  config.quote.refresh_threshold_ticks =
      integer_value<std::uint32_t>(result.parsed_values, "quotes.refresh_threshold_ticks");
  config.quote.maximum_distance_ticks =
      integer_value<std::uint32_t>(result.parsed_values, "quotes.maximum_distance_ticks");
  config.quote.tick_size_price4 =
      integer_value<std::uint32_t>(result.parsed_values, "quotes.tick_size_price4");
  config.risk.max_absolute_inventory =
      integer_value<std::int64_t>(result.parsed_values, "risk.max_absolute_inventory");
  config.risk.max_order_quantity =
      integer_value<std::uint64_t>(result.parsed_values, "risk.max_order_quantity");
  config.risk.max_open_orders =
      integer_value<std::uint64_t>(result.parsed_values, "risk.max_open_orders");
  config.risk.max_notional_nanos =
      integer_value<MoneyNanos>(result.parsed_values, "risk.max_notional_nanos");
  config.risk.max_market_staleness_ns =
      integer_value<TimestampNs>(result.parsed_values, "risk.max_market_staleness_ns");
  config.risk.maximum_loss_nanos =
      integer_value<MoneyNanos>(result.parsed_values, "risk.maximum_loss_nanos");
  config.risk.maximum_drawdown_nanos =
      integer_value<MoneyNanos>(result.parsed_values, "risk.maximum_drawdown_nanos");
  config.risk.stop_new_quotes_before_close_ns =
      integer_value<TimestampNs>(result.parsed_values, "risk.stop_new_quotes_before_close_ns");
  config.strategy.gamma = double_value(result.parsed_values, "strategy.gamma");
  config.strategy.sigma_squared = double_value(result.parsed_values, "strategy.sigma_squared");
  config.strategy.arrival_intensity_k =
      double_value(result.parsed_values, "strategy.arrival_intensity_k");
  config.strategy.symmetric_half_spread_price4 =
      double_value(result.parsed_values, "strategy.symmetric_half_spread_price4");
  config.strategy.signal_coefficient_price4 =
      double_value(result.parsed_values, "strategy.signal_coefficient_price4");
  config.strategy.session_end_ns =
      integer_value<TimestampNs>(result.parsed_values, "strategy.session_end_ns");
  config.strategy.fitted_partition = string_value(result.parsed_values, "calibration.fit_scope");
  if (config.strategy.fitted_partition != "train" ||
      bool_value(result.parsed_values, "calibration.validation_refit") ||
      bool_value(result.parsed_values, "calibration.test_refit")) {
    throw std::runtime_error("strategy calibration must be train-only");
  }
  config.markout_horizons_ns = integer_array(result.parsed_values, "markout.horizons_ns");
  config.random_seed = integer_value<std::uint64_t>(result.parsed_values, "random.seed");
  if (integer_value<std::uint32_t>(result.parsed_values, "protocol.schema_version") != 1 ||
      string_value(result.parsed_values, "latency.unit") != "ns" || config.quote.quantity == 0 ||
      config.quote.tick_size_price4 == 0 || config.risk.max_absolute_inventory <= 0 ||
      config.risk.max_order_quantity == 0 || config.risk.max_open_orders == 0 ||
      config.risk.max_notional_nanos <= 0 || config.risk.maximum_loss_nanos <= 0 ||
      config.risk.maximum_drawdown_nanos <= 0 || config.fees.maker_fee_nanos_per_share < 0 ||
      config.fees.maker_rebate_nanos_per_share < 0 ||
      config.fees.liquidation_fee_nanos_per_share < 0) {
    throw std::runtime_error("protocol contains an invalid bound or unit");
  }
  config.protocol_sha256 = result.sha256;
  config.strategy.fitted_protocol_sha256 = result.sha256;
  return result;
}

} // namespace lob::mm
