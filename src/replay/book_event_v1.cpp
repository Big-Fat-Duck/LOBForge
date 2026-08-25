#include "lob/replay/book_event_v1.hpp"

#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>

namespace lob::replay {
namespace {

std::string digest_hex(const std::uint64_t digest) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::hex << std::setfill('0') << std::setw(16) << digest;
  return out.str();
}

std::string json_escape(const std::string_view value) {
  std::string result;
  for (const char raw : value) {
    const auto character = static_cast<unsigned char>(raw);
    if (character == '"' || character == '\\') {
      result.push_back('\\');
      result.push_back(static_cast<char>(character));
    } else if (character < 0x20U) {
      std::ostringstream escaped;
      escaped.imbue(std::locale::classic());
      escaped << "\\u" << std::hex << std::setfill('0') << std::setw(4)
              << static_cast<unsigned int>(character);
      result += escaped.str();
    } else {
      result.push_back(static_cast<char>(character));
    }
  }
  return result;
}

template <typename Integer>
void append_optional_integer(std::ostringstream &out, const std::optional<Integer> &value) {
  if (value.has_value()) {
    out << *value;
  } else {
    out << "null";
  }
}

void append_depth(std::ostringstream &out, const std::vector<FactualDepthLevel> &levels) {
  out << '[';
  bool first = true;
  for (const auto &level : levels) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << '[' << level.price << ',' << level.shares << ']';
  }
  out << ']';
}

std::string_view action_for(const char type) noexcept {
  switch (type) {
  case 'A':
  case 'F':
    return "add";
  case 'E':
    return "execute";
  case 'C':
    return "execute_with_price";
  case 'X':
    return "cancel";
  case 'D':
    return "delete";
  case 'U':
    return "replace";
  default:
    return "unknown";
  }
}

std::uint64_t count_errors(const ReplayStatistics &statistics) {
  std::uint64_t count = 0;
  for (const auto &[category, occurrences] : statistics.error_counts) {
    (void)category;
    count += occurrences;
  }
  for (const auto &[category, occurrences] : statistics.warning_counts) {
    (void)category;
    count += occurrences;
  }
  return count;
}

bool leap_year(const int year) noexcept {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

} // namespace

bool valid_session_date(const std::string &value) noexcept {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index != 4 && index != 7 && (value[index] < '0' || value[index] > '9')) {
      return false;
    }
  }
  const int year = std::stoi(value.substr(0, 4));
  const int month = std::stoi(value.substr(5, 2));
  const int day = std::stoi(value.substr(8, 2));
  if (year == 0 || month < 1 || month > 12 || day < 1) {
    return false;
  }
  constexpr int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int maximum = month == 2 && leap_year(year) ? 29 : month_days[month - 1];
  return day <= maximum;
}

bool mutation_selected(const FactualState &state, const BookEventV1Options &options) {
  const auto &mutation = state.last_book_mutation();
  return mutation.has_value() &&
         (options.symbols.empty() || options.symbols.contains(mutation->symbol));
}

std::string render_book_event_v1_header(const BookEventV1Options &options,
                                        const std::size_t source_size) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"record_type\":\"header\",\"schema\":\"lobforge.book_event\",\"version\":1,"
      << "\"session_date\":\"" << options.session_date
      << "\",\"price_scale\":10000,\"timestamp_unit\":\"ns_since_midnight\",\"depth\":"
      << options.depth << ",\"source_size\":" << source_size << '}';
  return out.str();
}

std::string render_book_event_v1(const BookEventV1Options &options, const std::uint64_t sequence,
                                 const itch::Message &message, const itch::Frame &frame,
                                 const FactualState &state) {
  const auto &mutation = *state.last_book_mutation();
  const auto bids = state.depth(mutation.symbol, itch::FeedSide::Buy, options.depth);
  const auto asks = state.depth(mutation.symbol, itch::FeedSide::Sell, options.depth);
  const bool two_sided = !bids.empty() && !asks.empty();
  const bool locked = two_sided && bids.front().price == asks.front().price;
  const bool crossed = two_sided && bids.front().price > asks.front().price;
  const auto trading_state = state.trading_state(mutation.symbol);

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"record_type\":\"book_event\",\"session_date\":\"" << options.session_date
      << "\",\"sequence\":" << sequence << ",\"source_offset\":" << frame.envelope_offset
      << ",\"timestamp_ns\":" << itch::common_header(message).timestamp
      << ",\"stock_locate\":" << mutation.stock_locate << ",\"symbol\":\""
      << json_escape(mutation.symbol) << "\",\"message_type\":\"" << mutation.message_type
      << "\",\"action\":\"" << action_for(mutation.message_type) << "\",\"side\":\""
      << (mutation.side == itch::FeedSide::Buy ? 'B' : 'S')
      << "\",\"order_ref\":" << mutation.order_reference << ",\"new_order_ref\":";
  append_optional_integer(out, mutation.new_order_reference);
  out << ",\"event_qty\":" << mutation.event_quantity
      << ",\"display_price4\":" << mutation.display_price << ",\"execution_price4\":";
  append_optional_integer(out, mutation.execution_price);
  out << ",\"match_number\":";
  append_optional_integer(out, mutation.match_number);
  out << ",\"session_state\":\"" << to_string(state.session_phase()) << "\",\"trading_state\":";
  if (trading_state.has_value()) {
    out << '"' << *trading_state << '"';
  } else {
    out << "null";
  }
  out << ",\"two_sided\":" << (two_sided ? "true" : "false")
      << ",\"locked\":" << (locked ? "true" : "false")
      << ",\"crossed\":" << (crossed ? "true" : "false") << ",\"bids\":";
  append_depth(out, bids);
  out << ",\"asks\":";
  append_depth(out, asks);
  out << '}';
  return out.str();
}

std::string render_book_event_v1_summary(const Replayer &replayer,
                                         const std::uint64_t output_records) {
  const auto &statistics = replayer.statistics();
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"record_type\":\"summary\",\"schema\":\"lobforge.book_event\",\"version\":1,"
      << "\"input_bytes\":" << statistics.input_bytes
      << ",\"records_seen\":" << statistics.records_seen
      << ",\"records_decoded\":" << statistics.records_decoded
      << ",\"records_applied\":" << statistics.records_applied
      << ",\"records_output\":" << output_records
      << ",\"records_skipped\":" << statistics.records_skipped
      << ",\"errors\":" << count_errors(statistics) << ",\"factual_book_digest\":\""
      << digest_hex(replayer.state().digest()) << "\"}";
  return out.str();
}

} // namespace lob::replay
