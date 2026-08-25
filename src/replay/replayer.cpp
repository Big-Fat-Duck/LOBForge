#include "lob/replay/replayer.hpp"

#include <iomanip>
#include <sstream>

namespace lob::replay {
namespace {

std::string digest_hex(const std::uint64_t digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << digest;
  return out.str();
}

std::string json_escape(const std::string &value) {
  std::string result;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20U) {
        std::ostringstream escaped;
        escaped << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                << static_cast<unsigned int>(character);
        result += escaped.str();
      } else {
        result.push_back(static_cast<char>(character));
      }
    }
  }
  return result;
}

template <typename Map> std::uint64_t count_values(const Map &values) {
  std::uint64_t result = 0;
  for (const auto &[key, count] : values) {
    (void)key;
    result += count;
  }
  return result;
}

} // namespace

bool Replayer::run(const std::span<const std::byte> file, const ReplayMode mode,
                   const std::optional<std::size_t> max_messages, const ApplyObserver &observer) {
  state_.capture_book_mutations(static_cast<bool>(observer));
  statistics_.input_bytes = file.size();
  itch::FramedReader reader(file);
  while (!reader.done()) {
    if (max_messages.has_value() && statistics_.records_seen >= *max_messages) {
      break;
    }
    ++statistics_.records_seen;
    const auto frame = reader.next();
    if (!frame.ok()) {
      ++statistics_.records_failed;
      terminal_failure_ = true;
      record_diagnostic(*frame.error, mode, true);
      break;
    }
    const auto decoded = itch::decode_message(frame.frame->payload, frame.frame->payload_offset,
                                              frame.frame->record_index);
    if (!decoded.ok()) {
      ++statistics_.records_failed;
      ++statistics_.records_skipped;
      const bool terminal = mode == ReplayMode::Strict;
      terminal_failure_ = terminal;
      record_diagnostic(*decoded.error, mode, terminal);
      if (terminal) {
        break;
      }
      continue;
    }

    ++statistics_.records_decoded;
    const char type = itch::message_type(*decoded.message);
    ++statistics_.message_type_counts[type];
    const auto timestamp = itch::common_header(*decoded.message).timestamp;
    if (!statistics_.first_timestamp.has_value()) {
      statistics_.first_timestamp = timestamp;
    }
    statistics_.last_timestamp = timestamp;

    const auto applied =
        state_.apply(*decoded.message, frame.frame->payload_offset, frame.frame->record_index);
    if (!applied.ok()) {
      ++statistics_.records_failed;
      ++statistics_.records_skipped;
      const bool terminal = mode == ReplayMode::Strict;
      terminal_failure_ = terminal;
      record_diagnostic(*applied.error, mode, terminal);
      if (terminal) {
        break;
      }
      continue;
    }
    ++statistics_.records_applied;
    if (observer) {
      observer(*decoded.message, *frame.frame, state_);
    }
  }

  std::string invariant_error;
  if (!state_.check_invariants(&invariant_error)) {
    (void)invariant_error;
    invariant_failure_ = true;
    terminal_failure_ = true;
    itch::ParseError failure{itch::ErrorCategory::InvariantViolation,
                             reader.offset(),
                             reader.record_index(),
                             std::nullopt,
                             std::nullopt,
                             std::nullopt,
                             "factual state invariant failed"};
    record_diagnostic(failure, mode, true);
  }
  return !terminal_failure_ && !invariant_failure_;
}

void Replayer::record_diagnostic(const itch::ParseError &error, const ReplayMode mode,
                                 const bool terminal) {
  diagnostics_.push_back(error);
  if (terminal || mode == ReplayMode::Strict) {
    ++statistics_.error_counts[error.category];
  } else {
    ++statistics_.warning_counts[error.category];
  }
}

std::string Replayer::canonical_state() const {
  std::ostringstream out;
  out << state_.canonical_state() << "input_bytes=" << statistics_.input_bytes
      << ";records=" << statistics_.records_seen << ',' << statistics_.records_decoded << ','
      << statistics_.records_applied << ',' << statistics_.records_skipped << ','
      << statistics_.records_failed << ';';
  for (const auto &[type, count] : statistics_.message_type_counts) {
    out << "type=" << type << ':' << count << ';';
  }
  if (statistics_.first_timestamp.has_value()) {
    out << "timestamps=" << *statistics_.first_timestamp << ',' << *statistics_.last_timestamp
        << ';';
  }
  for (const auto &[category, count] : statistics_.warning_counts) {
    out << "warning=" << itch::to_string(category) << ':' << count << ';';
  }
  for (const auto &[category, count] : statistics_.error_counts) {
    out << "error=" << itch::to_string(category) << ':' << count << ';';
  }
  return out.str();
}

std::uint64_t Replayer::digest() const {
  constexpr std::uint64_t offset_basis = 1'469'598'103'934'665'603ULL;
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  std::uint64_t value = offset_basis;
  for (const char character : canonical_state()) {
    value ^= static_cast<unsigned char>(character);
    value *= prime;
  }
  return value;
}

std::string render_text(const Replayer &replayer, const std::optional<std::string> &symbol,
                        const std::size_t top_levels) {
  const auto &stats = replayer.statistics();
  const auto &state = replayer.state();
  const auto &volumes = state.volumes();
  std::ostringstream out;
  out << "lobforge_replay_v1\n"
      << "input_bytes=" << stats.input_bytes << '\n'
      << "records_seen=" << stats.records_seen << '\n'
      << "records_decoded=" << stats.records_decoded << '\n'
      << "records_applied=" << stats.records_applied << '\n'
      << "records_skipped=" << stats.records_skipped << '\n'
      << "records_failed=" << stats.records_failed << '\n'
      << "first_timestamp=";
  if (stats.first_timestamp.has_value()) {
    out << *stats.first_timestamp;
  } else {
    out << "null";
  }
  out << "\nlast_timestamp=";
  if (stats.last_timestamp.has_value()) {
    out << *stats.last_timestamp;
  } else {
    out << "null";
  }
  out << "\nsession_phase=" << to_string(state.session_phase()) << '\n'
      << "directory_symbols=" << state.symbol_count() << '\n'
      << "active_orders=" << state.active_order_count() << '\n'
      << "nonempty_price_levels=" << state.active_price_level_count() << '\n'
      << "displayed_add_volume=" << volumes.displayed_add << '\n'
      << "displayed_cancel_volume=" << volumes.displayed_cancel << '\n'
      << "displayed_execute_volume=" << volumes.displayed_execute << '\n'
      << "printable_trade_volume=" << volumes.printable_trade << '\n'
      << "non_printable_trade_volume=" << volumes.non_printable_trade << '\n'
      << "broken_trade_volume=" << volumes.broken_trade << '\n'
      << "warning_count=" << count_values(stats.warning_counts) << '\n'
      << "error_count=" << count_values(stats.error_counts) << '\n'
      << "state_digest_fnv1a64=" << digest_hex(replayer.digest()) << '\n'
      << "message_type_counts:\n";
  for (const auto &[type, count] : stats.message_type_counts) {
    out << "  " << type << ' ' << count << '\n';
  }
  out << "warning_categories:\n";
  for (const auto &[category, count] : stats.warning_counts) {
    out << "  " << itch::to_string(category) << ' ' << count << '\n';
  }
  out << "error_categories:\n";
  for (const auto &[category, count] : stats.error_counts) {
    out << "  " << itch::to_string(category) << ' ' << count << '\n';
  }
  out << "selected_symbol=" << (symbol.has_value() ? *symbol : "null") << '\n';
  if (symbol.has_value()) {
    out << "bids:\n";
    for (const auto &level : state.depth(*symbol, itch::FeedSide::Buy, top_levels)) {
      out << "  " << level.price << ' ' << level.shares << ' ' << level.order_count << '\n';
    }
    out << "asks:\n";
    for (const auto &level : state.depth(*symbol, itch::FeedSide::Sell, top_levels)) {
      out << "  " << level.price << ' ' << level.shares << ' ' << level.order_count << '\n';
    }
  }
  return out.str();
}

std::string render_json(const Replayer &replayer, const std::optional<std::string> &symbol,
                        const std::size_t top_levels) {
  const auto &stats = replayer.statistics();
  const auto &state = replayer.state();
  const auto &volumes = state.volumes();
  std::ostringstream out;
  out << "{";
  out << "\"schema\":\"lobforge_replay_v1\",";
  out << "\"input_bytes\":" << stats.input_bytes << ',';
  out << "\"records_seen\":" << stats.records_seen << ',';
  out << "\"records_decoded\":" << stats.records_decoded << ',';
  out << "\"records_applied\":" << stats.records_applied << ',';
  out << "\"records_skipped\":" << stats.records_skipped << ',';
  out << "\"records_failed\":" << stats.records_failed << ',';
  out << "\"first_timestamp\":";
  if (stats.first_timestamp.has_value()) {
    out << *stats.first_timestamp;
  } else {
    out << "null";
  }
  out << ",\"last_timestamp\":";
  if (stats.last_timestamp.has_value()) {
    out << *stats.last_timestamp;
  } else {
    out << "null";
  }
  out << ",\"session_phase\":\"" << to_string(state.session_phase()) << "\",";
  out << "\"directory_symbols\":" << state.symbol_count() << ',';
  out << "\"active_orders\":" << state.active_order_count() << ',';
  out << "\"nonempty_price_levels\":" << state.active_price_level_count() << ',';
  out << "\"volumes\":{";
  out << "\"displayed_add\":" << volumes.displayed_add << ',';
  out << "\"displayed_cancel\":" << volumes.displayed_cancel << ',';
  out << "\"displayed_execute\":" << volumes.displayed_execute << ',';
  out << "\"printable_trade\":" << volumes.printable_trade << ',';
  out << "\"non_printable_trade\":" << volumes.non_printable_trade << ',';
  out << "\"broken_trade\":" << volumes.broken_trade << "},";
  out << "\"message_type_counts\":{";
  bool first = true;
  for (const auto &[type, count] : stats.message_type_counts) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << '\"' << type << "\":" << count;
  }
  out << "},\"warning_categories\":{";
  first = true;
  for (const auto &[category, count] : stats.warning_counts) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << '\"' << itch::to_string(category) << "\":" << count;
  }
  out << "},\"error_categories\":{";
  first = true;
  for (const auto &[category, count] : stats.error_counts) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << '\"' << itch::to_string(category) << "\":" << count;
  }
  out << "},\"state_digest_fnv1a64\":\"" << digest_hex(replayer.digest()) << "\",";
  out << "\"selected_symbol\":";
  if (symbol.has_value()) {
    out << '\"' << json_escape(*symbol) << '\"';
  } else {
    out << "null";
  }
  out << ",\"bids\":[";
  first = true;
  if (symbol.has_value()) {
    for (const auto &level : state.depth(*symbol, itch::FeedSide::Buy, top_levels)) {
      if (!first) {
        out << ',';
      }
      first = false;
      out << "{\"price\":" << level.price << ",\"shares\":" << level.shares
          << ",\"orders\":" << level.order_count << '}';
    }
  }
  out << "],\"asks\":[";
  first = true;
  if (symbol.has_value()) {
    for (const auto &level : state.depth(*symbol, itch::FeedSide::Sell, top_levels)) {
      if (!first) {
        out << ',';
      }
      first = false;
      out << "{\"price\":" << level.price << ",\"shares\":" << level.shares
          << ",\"orders\":" << level.order_count << '}';
    }
  }
  out << "]}\n";
  return out.str();
}

} // namespace lob::replay
