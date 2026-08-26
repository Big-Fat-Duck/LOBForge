#include "lob/itch/decoder.hpp"
#include "lob/itch/framed_reader.hpp"
#include "lob/mm/audit.hpp"
#include "lob/mm/protocol.hpp"
#include "lob/mm/simulator.hpp"
#include "lob/replay/factual_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::optional<std::filesystem::path> input;
  std::optional<std::string> synthetic_fixture;
  std::filesystem::path config;
  std::filesystem::path output_dir;
  std::size_t event_chunk{65'536};
};

void usage() {
  std::cerr << "usage: lobforge_mm_sim (--input <itch-file>|--synthetic-fixture primary) "
               "--config <round4_protocol.toml> --output-dir <directory> "
               "[--event-chunk <positive-integer>]\n";
}

std::optional<Options> parse_options(const int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto take_value = [&]() -> std::optional<std::string> {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      return std::string{argv[++index]};
    };
    if (argument == "--input") {
      const auto value = take_value();
      if (!value.has_value() || options.input.has_value()) {
        return std::nullopt;
      }
      options.input = std::filesystem::path{*value};
    } else if (argument == "--synthetic-fixture") {
      const auto value = take_value();
      if (!value.has_value() || options.synthetic_fixture.has_value() || *value != "primary") {
        return std::nullopt;
      }
      options.synthetic_fixture = *value;
    } else if (argument == "--config") {
      const auto value = take_value();
      if (!value.has_value() || !options.config.empty()) {
        return std::nullopt;
      }
      options.config = *value;
    } else if (argument == "--output-dir") {
      const auto value = take_value();
      if (!value.has_value() || !options.output_dir.empty()) {
        return std::nullopt;
      }
      options.output_dir = *value;
    } else if (argument == "--event-chunk") {
      const auto value = take_value();
      if (!value.has_value()) {
        return std::nullopt;
      }
      std::size_t parsed = 0;
      for (const char character : *value) {
        const auto digit =
            character >= '0' && character <= '9' ? static_cast<std::size_t>(character - '0') : 10U;
        if (digit > 9U || parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
          return std::nullopt;
        }
        parsed = parsed * 10U + digit;
      }
      if (parsed == 0) {
        return std::nullopt;
      }
      options.event_chunk = parsed;
    } else {
      return std::nullopt;
    }
  }
  if (options.input.has_value() == options.synthetic_fixture.has_value() ||
      options.config.empty() || options.output_dir.empty()) {
    return std::nullopt;
  }
  return options;
}

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return std::nullopt;
  }
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) >
                     static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0, std::ios::beg);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  return input ? std::optional<std::vector<std::byte>>{std::move(bytes)} : std::nullopt;
}

std::string json_escape(const std::string_view value) {
  std::ostringstream output;
  for (const char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        output << "\\u00" << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<unsigned>(static_cast<unsigned char>(character)) << std::dec;
      } else {
        output << character;
      }
    }
  }
  return output.str();
}

template <std::size_t Size> lob::itch::FixedAscii<Size> fixed(const std::string_view value) {
  lob::itch::FixedAscii<Size> result;
  result.raw.fill(' ');
  std::copy_n(value.begin(), std::min(value.size(), Size), result.raw.begin());
  return result;
}

lob::itch::CommonHeader header(const lob::itch::StockLocate locate,
                               const lob::itch::FeedTimestamp timestamp) {
  return {locate, 1, timestamp};
}

std::vector<lob::itch::Message> synthetic_messages() {
  constexpr lob::itch::FeedTimestamp base = 36'000'000'000'000ULL;
  lob::itch::StockDirectory directory{};
  directory.header = header(1, 3);
  directory.stock = fixed<8>("SYNTH");
  directory.market_category = 'Q';
  directory.financial_status = 'N';
  directory.round_lot_size = 100;
  directory.round_lots_only = 'N';
  directory.issue_classification = 'C';
  directory.issue_sub_type = fixed<2>("");
  directory.authenticity = 'P';
  directory.short_sale_threshold_indicator = 'N';
  directory.ipo_flag = 'N';
  directory.luld_reference_price_tier = '1';
  directory.etp_flag = 'N';
  directory.etp_leverage_factor = 1;
  directory.inverse_indicator = 'N';
  const auto stock = fixed<8>("SYNTH");
  return {
      lob::itch::SystemEvent{header(0, 1), 'O'},
      lob::itch::SystemEvent{header(0, 2), 'S'},
      directory,
      lob::itch::SystemEvent{header(0, 4), 'Q'},
      lob::itch::StockTradingAction{header(1, base), stock, 'T', ' ', fixed<4>("")},
      lob::itch::AddOrder{header(1, base + 1), 10, lob::itch::FeedSide::Buy, 10, stock, 10'000},
      lob::itch::AddOrder{header(1, base + 2), 11, lob::itch::FeedSide::Sell, 10, stock, 10'200},
      lob::itch::RegShoRestriction{header(1, base + 1'000'000), stock, '0'},
      lob::itch::AddOrder{header(1, base + 2'000'000), 12, lob::itch::FeedSide::Buy, 10, stock,
                          10'000},
      lob::itch::OrderExecuted{header(1, base + 3'000'000), 10, 10, 1001},
      lob::itch::OrderExecuted{header(1, base + 4'000'000), 12, 5, 1002},
      lob::itch::OrderReplace{header(1, base + 14'000'000), 11, 13, 10, 10'300},
      lob::itch::AddOrder{header(1, base + 104'000'000), 14, lob::itch::FeedSide::Buy, 10, stock,
                          10'100},
      lob::itch::OrderReplace{header(1, base + 1'004'000'000), 13, 15, 10, 10'400},
      lob::itch::SystemEvent{header(0, base + 1'100'000'000), 'M'},
      lob::itch::SystemEvent{header(0, base + 1'100'000'001), 'E'},
      lob::itch::SystemEvent{header(0, base + 1'100'000'002), 'C'},
  };
}

bool apply_message(lob::mm::ShadowSimulator &simulator, lob::replay::FactualState &state,
                   const lob::itch::Message &message, const std::uint64_t sequence,
                   std::string &error) {
  const auto timestamp = lob::itch::common_header(message).timestamp;
  simulator.before_market(timestamp, sequence, state);
  const auto applied = state.apply(message, 0, static_cast<std::size_t>(sequence - 1));
  if (!applied.ok()) {
    error = "factual apply error category=" +
            std::string(lob::itch::to_string(applied.error->category));
    return false;
  }
  simulator.after_market(message, sequence, state);
  std::string invariant;
  if (!state.check_invariants(&invariant)) {
    error = "factual invariant failure: " + invariant;
    return false;
  }
  if (!simulator.check_invariants(&invariant)) {
    error = "shadow invariant failure: " + invariant;
    return false;
  }
  return true;
}

bool run_bytes(const std::vector<std::byte> &bytes, lob::mm::ShadowSimulator &simulator,
               lob::replay::FactualState &state, std::uint64_t &sequence,
               lob::mm::TimestampNs &last_timestamp, const std::size_t event_chunk,
               std::string &error) {
  lob::itch::FramedReader reader(bytes);
  while (!reader.done()) {
    for (std::size_t in_chunk = 0; in_chunk < event_chunk && !reader.done(); ++in_chunk) {
      const auto frame = reader.next();
      if (!frame.ok()) {
        error =
            "framing error category=" + std::string(lob::itch::to_string(frame.error->category));
        return false;
      }
      const auto decoded = lob::itch::decode_message(
          frame.frame->payload, frame.frame->payload_offset, frame.frame->record_index);
      if (!decoded.ok()) {
        error =
            "decode error category=" + std::string(lob::itch::to_string(decoded.error->category));
        return false;
      }
      ++sequence;
      last_timestamp = lob::itch::common_header(*decoded.message).timestamp;
      if (!apply_message(simulator, state, *decoded.message, sequence, error)) {
        return false;
      }
    }
  }
  return true;
}

bool write_text(const std::filesystem::path &path, const std::string &content) {
  std::ofstream output(path, std::ios::binary);
  output << content;
  return static_cast<bool>(output);
}

template <typename Values, typename Renderer>
bool write_ndjson(const std::filesystem::path &path, const Values &values, Renderer renderer) {
  std::ofstream output(path, std::ios::binary);
  for (const auto &value : values) {
    output << renderer(value) << '\n';
  }
  return static_cast<bool>(output);
}

std::string manifest_json(const lob::mm::FrozenProtocol &protocol, const std::string &source_kind,
                          const std::string &source_name, const std::string &source_sha256,
                          const std::uint64_t source_size,
                          const lob::mm::SimulationSummary &summary,
                          const lob::replay::FactualState &state, const std::size_t event_chunk) {
  std::ostringstream output;
  output << "{\"schema\":\"lobforge.mm_manifest\",\"version\":1,"
            "\"source_kind\":\""
         << source_kind << "\",\"source_name\":\"" << json_escape(source_name)
         << "\",\"source_sha256\":\"" << source_sha256 << "\",\"source_size\":" << source_size
         << ",\"protocol_sha256\":\"" << protocol.sha256 << "\",\"event_chunk\":" << event_chunk
         << ",\"replay_digest\":\"" << std::hex << state.digest() << std::dec
         << "\",\"replay_binary_version\":\"lobforge_mm_sim_v1\","
            "\"replay_commit\":\"unknown\",\"order_rows\":"
         << summary.order_event_rows << ",\"fill_rows\":" << summary.fill_events
         << ",\"inventory_rows\":" << summary.inventory_event_rows << ",\"semantic_digest\":\""
         << summary.semantic_digest
         << "\",\"determinism_basis\":\"canonical_typed_logical_records\","
            "\"real_data_evidence\":\"BLOCKED: DATASET_NOT_PROVIDED\"}";
  return output.str();
}

} // namespace

int main(int argc, char **argv) {
  std::ios::sync_with_stdio(false);
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << "lobforge_mm_sim_v1\n";
    return 0;
  }
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    usage();
    return 2;
  }

  try {
    const auto protocol = lob::mm::load_protocol(options->config);
    lob::mm::ShadowSimulator simulator(protocol.simulation);
    lob::replay::FactualState state;
    state.capture_book_mutations(true);
    std::uint64_t sequence = 0;
    lob::mm::TimestampNs last_timestamp = 0;
    std::string source_kind;
    std::string source_name;
    std::string source_sha256;
    std::uint64_t source_size = 0;
    std::string error;
    if (options->input.has_value()) {
      const auto bytes = read_file(*options->input);
      if (!bytes.has_value()) {
        std::cerr << "lobforge_mm_sim: unable to read input file\n";
        return 3;
      }
      source_kind = "itch_binary";
      source_name = options->input->filename().string();
      source_size = bytes->size();
      source_sha256 = lob::mm::sha256_hex(
          std::string_view{reinterpret_cast<const char *>(bytes->data()), bytes->size()});
      if (!run_bytes(*bytes, simulator, state, sequence, last_timestamp, options->event_chunk,
                     error)) {
        std::cerr << "lobforge_mm_sim: " << error << '\n';
        return 4;
      }
    } else {
      source_kind = "deterministic_synthetic_fixture";
      source_name = *options->synthetic_fixture;
      source_sha256 = lob::mm::sha256_hex("lobforge-round4-synthetic-primary-v1");
      const auto messages = synthetic_messages();
      source_size = messages.size();
      for (std::size_t begin = 0; begin < messages.size(); begin += options->event_chunk) {
        const auto end = std::min(messages.size(), begin + options->event_chunk);
        for (std::size_t index = begin; index < end; ++index) {
          ++sequence;
          last_timestamp = lob::itch::common_header(messages[index]).timestamp;
          if (!apply_message(simulator, state, messages[index], sequence, error)) {
            std::cerr << "lobforge_mm_sim: " << error << '\n';
            return 4;
          }
        }
      }
    }
    simulator.finish(last_timestamp, state);
    if (!simulator.check_invariants(&error)) {
      std::cerr << "lobforge_mm_sim: shadow invariant failure: " << error << '\n';
      return 5;
    }
    const auto summary = simulator.summary();
    const auto temporary = options->output_dir.string() + ".tmp";
    const std::filesystem::path temporary_path{temporary};
    if (std::filesystem::exists(options->output_dir) || std::filesystem::exists(temporary_path) ||
        !std::filesystem::create_directories(temporary_path)) {
      std::cerr << "lobforge_mm_sim: output directory already exists or cannot be created\n";
      return 3;
    }
    const bool written =
        write_text(temporary_path / "protocol.json",
                   lob::mm::render_protocol_json(protocol) + "\n") &&
        write_ndjson(temporary_path / "shadow_orders.ndjson", simulator.order_events(),
                     [](const auto &value) { return lob::mm::render_json(value); }) &&
        write_ndjson(temporary_path / "shadow_fills.ndjson", simulator.fills(),
                     [](const auto &value) { return lob::mm::render_json(value); }) &&
        write_ndjson(temporary_path / "inventory_events.ndjson", simulator.inventory_events(),
                     [](const auto &value) { return lob::mm::render_json(value); }) &&
        write_text(temporary_path / "summary.json", lob::mm::render_json(summary) + "\n") &&
        write_text(temporary_path / "metrics.json", lob::mm::render_json(summary) + "\n") &&
        write_text(temporary_path / "manifest.json",
                   manifest_json(protocol, source_kind, source_name, source_sha256, source_size,
                                 summary, state, options->event_chunk) +
                       "\n");
    if (!written) {
      std::filesystem::remove_all(temporary_path);
      std::cerr << "lobforge_mm_sim: unable to write output artifacts\n";
      return 3;
    }
    std::filesystem::rename(temporary_path, options->output_dir);
    std::cout << "semantic_digest=" << summary.semantic_digest << '\n';
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "lobforge_mm_sim: " << exception.what() << '\n';
    return 3;
  }
}
