#include "lob/replay/book_event_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::string input;
  lob::replay::ReplayMode mode{lob::replay::ReplayMode::Strict};
  bool mode_explicit{};
  std::vector<std::string> symbols;
  std::size_t top{10};
  bool top_explicit{};
  std::string format{"text"};
  bool format_explicit{};
  std::optional<std::size_t> max_messages;
  std::optional<std::string> research_format;
  std::optional<std::string> session_date;
  std::size_t depth{10};
  bool depth_explicit{};
  std::optional<std::string> output;
};

void usage() {
  std::cerr << "usage: lobforge_replay --input <file> [--strict|--permissive] "
               "[--symbol <symbol>] [--top <N>] [--format text|json] "
               "[--max-messages <N>] [--session-date YYYY-MM-DD] "
               "[--research-format book-event-v1] [--depth <1-10>] [--output -]\n";
}

std::optional<std::size_t> parse_positive(const std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::size_t>(character - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return value == 0 ? std::nullopt : std::optional<std::size_t>{value};
}

std::optional<Options> parse_options(const int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto take_value = [&]() -> std::optional<std::string_view> {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      return std::string_view{argv[++index]};
    };
    if (argument == "--input") {
      const auto value = take_value();
      if (!value.has_value() || !options.input.empty()) {
        return std::nullopt;
      }
      options.input = *value;
    } else if (argument == "--strict" || argument == "--permissive") {
      if (options.mode_explicit) {
        return std::nullopt;
      }
      options.mode_explicit = true;
      options.mode = argument == "--strict" ? lob::replay::ReplayMode::Strict
                                            : lob::replay::ReplayMode::Permissive;
    } else if (argument == "--symbol") {
      const auto value = take_value();
      if (!value.has_value() || value->empty()) {
        return std::nullopt;
      }
      options.symbols.emplace_back(*value);
    } else if (argument == "--top") {
      const auto value = take_value();
      if (!value.has_value() || options.top_explicit) {
        return std::nullopt;
      }
      options.top_explicit = true;
      const auto parsed = parse_positive(*value);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.top = *parsed;
    } else if (argument == "--format") {
      const auto value = take_value();
      if (!value.has_value() || options.format_explicit || (*value != "text" && *value != "json")) {
        return std::nullopt;
      }
      options.format_explicit = true;
      options.format = *value;
    } else if (argument == "--max-messages") {
      const auto value = take_value();
      if (!value.has_value() || options.max_messages.has_value()) {
        return std::nullopt;
      }
      options.max_messages = parse_positive(*value);
      if (!options.max_messages.has_value()) {
        return std::nullopt;
      }
    } else if (argument == "--research-format") {
      const auto value = take_value();
      if (!value.has_value() || options.research_format.has_value() || *value != "book-event-v1") {
        return std::nullopt;
      }
      options.research_format = std::string{*value};
    } else if (argument == "--session-date") {
      const auto value = take_value();
      if (!value.has_value() || options.session_date.has_value() ||
          !lob::replay::valid_session_date(std::string{*value})) {
        return std::nullopt;
      }
      options.session_date = std::string{*value};
    } else if (argument == "--depth") {
      const auto value = take_value();
      if (!value.has_value() || options.depth_explicit) {
        return std::nullopt;
      }
      const auto parsed = parse_positive(*value);
      if (!parsed.has_value() || *parsed > 10) {
        return std::nullopt;
      }
      options.depth_explicit = true;
      options.depth = *parsed;
    } else if (argument == "--output") {
      const auto value = take_value();
      if (!value.has_value() || options.output.has_value() || *value != "-") {
        return std::nullopt;
      }
      options.output = std::string{*value};
    } else {
      return std::nullopt;
    }
  }
  if (options.input.empty()) {
    return std::nullopt;
  }
  if (options.research_format.has_value()) {
    if (!options.session_date.has_value() || !options.output.has_value() || options.top_explicit ||
        options.format_explicit) {
      return std::nullopt;
    }
  } else if (options.session_date.has_value() || options.depth_explicit ||
             options.output.has_value() || options.symbols.size() > 1) {
    return std::nullopt;
  }
  return options;
}

std::optional<std::vector<std::byte>> read_file(const std::string &path) {
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
    if (!input) {
      return std::nullopt;
    }
  }
  return bytes;
}

} // namespace

int main(int argc, char **argv) {
  std::ios::sync_with_stdio(false);
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << "lobforge_replay_v1\n";
    return 0;
  }
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    usage();
    return 2;
  }
  const auto bytes = read_file(options->input);
  if (!bytes.has_value()) {
    std::cerr << "lobforge_replay: unable to read input file\n";
    return 3;
  }

  lob::replay::Replayer replayer;
  bool success = false;
  if (options->research_format.has_value()) {
    lob::replay::BookEventV1Options export_options{
        *options->session_date, options->depth,
        std::set<std::string>{options->symbols.begin(), options->symbols.end()}};
    std::cout << lob::replay::render_book_event_v1_header(export_options, bytes->size()) << '\n';
    std::uint64_t output_records = 0;
    const lob::replay::ApplyObserver observer = [&](const lob::itch::Message &message,
                                                    const lob::itch::Frame &frame,
                                                    const lob::replay::FactualState &state) {
      if (!lob::replay::mutation_selected(state, export_options)) {
        return;
      }
      ++output_records;
      std::cout << lob::replay::render_book_event_v1(export_options, output_records, message, frame,
                                                     state)
                << '\n';
    };
    success = replayer.run(*bytes, options->mode, options->max_messages, observer);
    std::cout << lob::replay::render_book_event_v1_summary(replayer, output_records) << '\n';
  } else {
    success = replayer.run(*bytes, options->mode, options->max_messages);
    const std::optional<std::string> symbol =
        options->symbols.empty() ? std::nullopt
                                 : std::optional<std::string>{options->symbols.front()};
    if (options->format == "json") {
      std::cout << lob::replay::render_json(replayer, symbol, options->top);
    } else {
      std::cout << lob::replay::render_text(replayer, symbol, options->top);
    }
  }
  for (const auto &diagnostic : replayer.diagnostics()) {
    std::cerr << "diagnostic category=" << lob::itch::to_string(diagnostic.category)
              << " offset=" << diagnostic.absolute_file_offset
              << " record=" << diagnostic.record_index << " type=";
    if (diagnostic.message_type.has_value()) {
      std::cerr << *diagnostic.message_type;
    } else {
      std::cerr << "unknown";
    }
    std::cerr << " detail=" << diagnostic.diagnostic << '\n';
  }
  if (replayer.invariant_failure()) {
    return 5;
  }
  if (!success) {
    return 4;
  }
  if (!std::cout) {
    return 3;
  }
  return 0;
}
