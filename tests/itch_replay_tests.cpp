#include "lob/itch/byte_reader.hpp"
#include "lob/itch/decoder.hpp"
#include "lob/itch/framed_reader.hpp"
#include "lob/replay/book_event_v1.hpp"
#include "lob/replay/factual_book.hpp"
#include "lob/replay/replayer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef LOB_SOURCE_DIR
#define LOB_SOURCE_DIR "."
#endif

namespace {

using namespace lob;

class TestFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      throw TestFailure(std::string{"CHECK failed: "} + #condition + " at " + __FILE__ + ":" +     \
                        std::to_string(__LINE__));                                                 \
    }                                                                                              \
  } while (false)

struct Golden {
  char type;
  std::string_view hex;
  std::string_view canonical;
};

constexpr std::array<Golden, 23> kGolden{{
    {'S', "53000000020102030405064F", "S|0|2|1108152157446|O"},
    {'R', "52000100020102030405064141504C20202020464E000000644E432020504E5A314E000000014E",
     "R|1|2|1108152157446|4141504C20202020|F|N|100|N|C|2020|P|N|Z|1|N|1|N"},
    {'H', "48000100020102030405064141504C20202020542020202020",
     "H|1|2|1108152157446|4141504C20202020|T| |20202020"},
    {'Y', "59000100020102030405064141504C2020202030", "Y|1|2|1108152157446|4141504C20202020|0"},
    {'L', "4C00010002010203040506414243444141504C20202020594E41",
     "L|1|2|1108152157446|41424344|4141504C20202020|Y|N|A"},
    {'V', "56000000020102030405060000000005F5E100000000000BEBC2000000000011E1A300",
     "V|0|2|1108152157446|100000000|200000000|300000000"},
    {'W', "570000000201020304050631", "W|0|2|1108152157446|1"},
    {'K', "4B00000002010203040506544553542020202000008598410012D644",
     "K|0|2|1108152157446|5445535420202020|34200|A|1234500"},
    {'J', "4A000100020102030405064141504C20202020000F42400010C8E0000DBBA000000002",
     "J|1|2|1108152157446|4141504C20202020|1000000|1100000|900000|2"},
    {'h', "68000100020102030405064141504C202020205148", "h|1|2|1108152157446|4141504C20202020|Q|H"},
    {'A', "4100010002010203040506010203040506070842000000644141504C202020200012D644",
     "A|1|2|1108152157446|72623859790382856|B|100|4141504C20202020|1234500"},
    {'F', "4600010002010203040506111213141516171853000000C84141504C202020200012D6A841424344",
     "F|1|2|1108152157446|1230066625199609624|S|200|4141504C20202020|1234600|41424344"},
    {'E', "45000100020102030405060102030405060708000000192122232425262728",
     "E|1|2|1108152157446|72623859790382856|25|2387509390608836392"},
    {'C', "430001000201020304050611121314151617180000001E31323334353637384E0012D676",
     "C|1|2|1108152157446|1230066625199609624|30|3544952156018063160|N|1234550"},
    {'X', "580001000201020304050601020304050607080000000A",
     "X|1|2|1108152157446|72623859790382856|10"},
    {'D', "44000100020102030405061112131415161718", "D|1|2|1108152157446|1230066625199609624"},
    {'U', "550001000201020304050601020304050607084142434445464748000000500012D5E0",
     "U|1|2|1108152157446|72623859790382856|4702394921427289928|80|1234400"},
    {'P',
     "5000010002010203040506000000000000000042000000324141504C202020200012D6445152535455565758",
     "P|1|2|1108152157446|0|B|50|4141504C20202020|1234500|5859837686836516696"},
    {'Q', "510001000201020304050600000000000003E84141504C202020200012D64461626364656667684F",
     "Q|1|2|1108152157446|1000|4141504C20202020|1234500|7017280452245743464|O"},
    {'B', "42000100020102030405065152535455565758", "B|1|2|1108152157446|5859837686836516696"},
    {'I',
     "490001000201020304050600000000000003E800000000000000C8424141504C2020202000124F800012769000129"
     "DA04F4C",
     "I|1|2|1108152157446|1000|200|B|4141504C20202020|1200000|1210000|1220000|O|L"},
    {'N', "4E000100020102030405064141504C2020202042", "N|1|2|1108152157446|4141504C20202020|B"},
    {'O',
     "4F000100020102030405064141504C2020202059000F4240001E84800016E36000000000075BCD1500155CC000186"
     "A00",
     "O|1|2|1108152157446|4141504C20202020|Y|1000000|2000000|1500000|123456789|1400000|1600000"},
}};

std::vector<std::byte> from_hex(const std::string_view hex) {
  CHECK((hex.size() % 2) == 0);
  const auto nibble = [](const char character) -> unsigned int {
    if (character >= '0' && character <= '9') {
      return static_cast<unsigned int>(character - '0');
    }
    if (character >= 'A' && character <= 'F') {
      return 10U + static_cast<unsigned int>(character - 'A');
    }
    if (character >= 'a' && character <= 'f') {
      return 10U + static_cast<unsigned int>(character - 'a');
    }
    throw TestFailure("invalid hex digit");
  };
  std::vector<std::byte> result;
  result.reserve(hex.size() / 2);
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    result.push_back(static_cast<std::byte>((nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
  }
  return result;
}

std::vector<std::byte> framed(std::span<const std::byte> payload) {
  CHECK(payload.size() <= 65'535);
  std::vector<std::byte> result;
  result.reserve(payload.size() + 2);
  result.push_back(static_cast<std::byte>((payload.size() >> 8U) & 0xFFU));
  result.push_back(static_cast<std::byte>(payload.size() & 0xFFU));
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

template <std::size_t N> itch::FixedAscii<N> fixed(const std::string_view text) {
  itch::FixedAscii<N> result;
  result.raw.fill(' ');
  CHECK(text.size() <= N);
  std::copy(text.begin(), text.end(), result.raw.begin());
  return result;
}

itch::CommonHeader header(const itch::StockLocate locate, const std::uint64_t timestamp = 1) {
  return itch::CommonHeader{locate, 1, timestamp};
}

itch::StockDirectory directory(const itch::StockLocate locate, const std::string_view symbol) {
  itch::StockDirectory value{};
  value.header = header(locate);
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

itch::AddOrder add_order(const itch::StockLocate locate, const std::string_view symbol,
                         const itch::OrderReference reference, const itch::FeedSide side,
                         const std::uint32_t shares, const itch::Price4 price,
                         const std::uint64_t timestamp = 2) {
  return itch::AddOrder{header(locate, timestamp), reference, side, shares,
                        fixed<8>(symbol),          price};
}

void apply_ok(replay::FactualState &state, const itch::Message &message) {
  const auto result = state.apply(message);
  if (!result.ok()) {
    throw TestFailure("unexpected apply error: " +
                      std::string(itch::to_string(result.error->category)));
  }
  std::string invariant_error;
  CHECK(state.check_invariants(&invariant_error));
}

void byte_reader_endian_and_ascii() {
  const auto bytes = from_hex("0102030405060708");
  itch::ByteReader reader(bytes);
  std::uint16_t u16 = 0;
  std::uint32_t u32 = 0;
  std::uint64_t u48 = 0;
  CHECK(reader.read_be16(u16) && u16 == 0x0102U);
  CHECK(reader.read_be32(u32) && u32 == 0x03040506U);
  CHECK(reader.read_be48(u48) == false);
  CHECK(reader.position() == 6);
  std::uint16_t tail = 0;
  CHECK(reader.read_be16(tail) && tail == 0x0708U);
  const auto ascii_bytes = from_hex("4142432020");
  itch::ByteReader ascii_reader(ascii_bytes);
  itch::FixedAscii<5> ascii;
  CHECK(ascii_reader.read_ascii(ascii));
  CHECK(ascii.trimmed() == "ABC");
  std::uint8_t missing = 0;
  CHECK(!ascii_reader.read_u8(missing));

  const auto wide = from_hex("0102030405060708");
  itch::ByteReader wide_reader(wide);
  std::uint64_t u64 = 0;
  CHECK(wide_reader.read_be64(u64) && u64 == 0x0102030405060708ULL);
}

void golden_protocol_all_23_every_field() {
  for (const auto &fixture : kGolden) {
    const auto bytes = from_hex(fixture.hex);
    CHECK(itch::expected_payload_length(fixture.type) == bytes.size());
    const auto decoded = itch::decode_message(bytes, 100, 7);
    CHECK(decoded.ok());
    CHECK(itch::message_type(*decoded.message) == fixture.type);
    CHECK(itch::message_canonical(*decoded.message) == fixture.canonical);
    CHECK(itch::common_header(*decoded.message).timestamp == 0x010203040506ULL);
  }
}

void every_truncated_prefix_and_wrong_length() {
  for (const auto &fixture : kGolden) {
    const auto bytes = from_hex(fixture.hex);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
      const auto decoded =
          itch::decode_message(std::span<const std::byte>{bytes}.first(length), 50, 3);
      CHECK(!decoded.ok());
      CHECK(decoded.error->actual_length == length);
      CHECK(decoded.error->category == (length == 0 ? itch::ErrorCategory::EmptyPayload
                                                    : itch::ErrorCategory::LengthMismatch));
    }
    auto longer = bytes;
    longer.push_back(std::byte{0});
    const auto decoded = itch::decode_message(longer);
    CHECK(!decoded.ok() && decoded.error->category == itch::ErrorCategory::LengthMismatch);
  }
}

void malformed_enums_unknown_type_and_framing() {
  auto add = from_hex(kGolden[10].hex);
  add[19] = std::byte{'Z'};
  auto decoded = itch::decode_message(add, 200, 4);
  CHECK(!decoded.ok() && decoded.error->category == itch::ErrorCategory::InvalidEnum);
  CHECK(decoded.error->absolute_file_offset == 219);
  auto unknown = from_hex("5A00000000000000000000");
  decoded = itch::decode_message(unknown);
  CHECK(!decoded.ok() && decoded.error->category == itch::ErrorCategory::UnknownMessageType);

  const auto incomplete = from_hex("00");
  itch::FramedReader incomplete_reader(incomplete);
  const auto incomplete_result = incomplete_reader.next();
  CHECK(!incomplete_result.ok());
  CHECK(incomplete_result.error->category == itch::ErrorCategory::IncompleteEnvelope);

  const auto overrun = from_hex("000C5300");
  itch::FramedReader overrun_reader(overrun);
  const auto overrun_result = overrun_reader.next();
  CHECK(!overrun_result.ok());
  CHECK(overrun_result.error->category == itch::ErrorCategory::TruncatedFrame);
  CHECK(overrun_result.error->expected_length == 12);
  CHECK(overrun_result.error->actual_length == 2);
}

void add_execute_cancel_final_execute() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 10, itch::FeedSide::Buy, 100, 1'000'000)});
  apply_ok(state, itch::Message{itch::OrderExecuted{header(1, 3), 10, 25, 1001}});
  apply_ok(state, itch::Message{itch::OrderCancel{header(1, 4), 10, 30}});
  apply_ok(state, itch::Message{itch::OrderExecuted{header(1, 5), 10, 45, 1002}});
  CHECK(state.active_order_count() == 0);
  CHECK(state.volumes().displayed_add == 100);
  CHECK(state.volumes().displayed_cancel == 30);
  CHECK(state.volumes().displayed_execute == 70);
}

void execution_price_does_not_move_display_order() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 10, itch::FeedSide::Sell, 100, 1'000'000)});
  apply_ok(state,
           itch::Message{itch::OrderExecutedWithPrice{header(1, 3), 10, 20, 2001, 'Y', 1'100'000}});
  CHECK(state.order(10)->display_price == 1'000'000);
  CHECK(state.order(10)->remaining_shares == 80);
  CHECK(state.trades().back().execution_price == 1'100'000);
  CHECK(state.depth("AAPL", itch::FeedSide::Sell).front().price == 1'000'000);
}

void delete_and_replace_inherit_identity_and_new_priority() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  itch::AddOrderMpid attributed{header(1, 2),     10,        itch::FeedSide::Buy, 100,
                                fixed<8>("AAPL"), 1'000'000, fixed<4>("ABCD")};
  apply_ok(state, itch::Message{attributed});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 11, itch::FeedSide::Buy, 100, 1'000'000)});
  const auto old_sequence = state.order(10)->fifo_sequence;
  apply_ok(state, itch::Message{itch::OrderReplace{header(1, 4), 10, 12, 80, 1'000'000}});
  CHECK(!state.order(10).has_value());
  CHECK(state.order(12)->symbol == "AAPL");
  CHECK(state.order(12)->side == itch::FeedSide::Buy);
  CHECK(state.order(12)->attribution == "ABCD");
  CHECK(state.order(12)->fifo_sequence > old_sequence);
  CHECK(state.order(12)->queue_position == 1);
  apply_ok(state, itch::Message{itch::OrderDelete{header(1, 5), 11}});
  CHECK(!state.order(11).has_value());
  CHECK(state.order(12)->queue_position == 0);
}

void fifo_levels_both_sides_and_multi_symbol_isolation() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  apply_ok(state, itch::Message{directory(2, "MSFT")});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 1, itch::FeedSide::Buy, 10, 100)});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 2, itch::FeedSide::Buy, 20, 100)});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 3, itch::FeedSide::Buy, 30, 99)});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 4, itch::FeedSide::Sell, 40, 101)});
  apply_ok(state, itch::Message{add_order(2, "MSFT", 5, itch::FeedSide::Sell, 50, 200)});
  const auto bids = state.depth("AAPL", itch::FeedSide::Buy);
  const replay::FactualDepthLevel expected_best_bid{100, 30, 2};
  CHECK(bids.size() == 2 && bids[0] == expected_best_bid);
  CHECK(state.order(1)->queue_position == 0 && state.order(2)->queue_position == 1);
  CHECK(state.depth("MSFT", itch::FeedSide::Sell).front().shares == 50);
  CHECK(state.symbol_for_locate(1) == "AAPL" && state.symbol_for_locate(2) == "MSFT");
}

void semantic_errors_are_typed_and_atomic() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  apply_ok(state, itch::Message{directory(2, "MSFT")});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 1, itch::FeedSide::Buy, 10, 100)});
  const std::string before = state.canonical_state();
  auto result = state.apply(itch::Message{add_order(1, "AAPL", 1, itch::FeedSide::Buy, 10, 100)});
  CHECK(!result.ok() && result.error->category == itch::ErrorCategory::DuplicateOrderReference);
  CHECK(state.canonical_state() == before);
  result = state.apply(itch::Message{itch::OrderCancel{header(1), 99, 1}});
  CHECK(!result.ok() && result.error->category == itch::ErrorCategory::UnknownOrderReference);
  result = state.apply(itch::Message{itch::OrderCancel{header(2), 1, 1}});
  CHECK(!result.ok() && result.error->category == itch::ErrorCategory::StockLocateMismatch);
  result = state.apply(itch::Message{itch::OrderCancel{header(1), 1, 11}});
  CHECK(!result.ok() && result.error->category == itch::ErrorCategory::QuantityExceedsRemaining);
  result = state.apply(itch::Message{itch::OrderExecuted{header(1), 1, 11, 7}});
  CHECK(!result.ok() && result.error->category == itch::ErrorCategory::QuantityExceedsRemaining);
  result = state.apply(itch::Message{itch::OrderReplace{header(1), 1, 1, 5, 100}});
  CHECK(!result.ok() &&
        result.error->category == itch::ErrorCategory::DuplicateReplacementReference);
  CHECK(state.canonical_state() == before);
}

void trades_crosses_breaks_and_printability_do_not_change_depth() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 1, itch::FeedSide::Buy, 100, 100)});
  const auto depth_before = state.depth("AAPL", itch::FeedSide::Buy);
  apply_ok(state, itch::Message{itch::NonCrossTrade{header(1), 0, itch::FeedSide::Buy, 20,
                                                    fixed<8>("AAPL"), 101, 10}});
  apply_ok(state, itch::Message{itch::CrossTrade{header(1), 30, fixed<8>("AAPL"), 102, 11, 'O'}});
  apply_ok(state, itch::Message{itch::OrderExecutedWithPrice{header(1), 1, 10, 12, 'N', 103}});
  const auto depth_after_execution = state.depth("AAPL", itch::FeedSide::Buy);
  CHECK(depth_after_execution.front().shares == 90);
  apply_ok(state, itch::Message{itch::BrokenTrade{header(1), 10}});
  CHECK(state.depth("AAPL", itch::FeedSide::Buy) == depth_after_execution);
  CHECK(depth_before.front().shares == 100);
  CHECK(state.volumes().printable_trade == 50);
  CHECK(state.volumes().non_printable_trade == 10);
  CHECK(state.volumes().broken_trade == 20);
}

void valid_session_lifecycle_with_late_delete_and_break() {
  replay::FactualState state;
  for (const char code : std::string{"OSQME"}) {
    apply_ok(state, itch::Message{itch::SystemEvent{header(0), code}});
  }
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 1, itch::FeedSide::Buy, 10, 100)});
  apply_ok(state, itch::Message{itch::NonCrossTrade{header(1), 0, itch::FeedSide::Buy, 5,
                                                    fixed<8>("AAPL"), 100, 77}});
  apply_ok(state, itch::Message{itch::OrderDelete{header(1), 1}});
  apply_ok(state, itch::Message{itch::BrokenTrade{header(1), 77}});
  apply_ok(state, itch::Message{itch::SystemEvent{header(0), 'C'}});
  CHECK(state.session_phase() == replay::SessionPhase::MessagesEnded);
}

void uppercase_h_lowercase_h_and_o_distinctions() {
  const auto trading = itch::decode_message(from_hex(kGolden[2].hex));
  const auto halt = itch::decode_message(from_hex(kGolden[9].hex));
  const auto system = itch::decode_message(from_hex(kGolden[0].hex));
  const auto dlcr = itch::decode_message(from_hex(kGolden[22].hex));
  CHECK(std::holds_alternative<itch::StockTradingAction>(*trading.message));
  CHECK(std::holds_alternative<itch::OperationalHalt>(*halt.message));
  CHECK(std::get<itch::SystemEvent>(*system.message).event_code == 'O');
  CHECK(std::holds_alternative<itch::DlcrPriceDiscovery>(*dlcr.message));
}

std::vector<std::byte> load_full_session_fixture() {
  const std::filesystem::path path =
      std::filesystem::path{LOB_SOURCE_DIR} / "tests" / "fixtures" / "synthetic_full_session.hex";
  std::ifstream input(path);
  CHECK(input.good());
  std::vector<std::byte> bytes;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto record = from_hex(line);
    bytes.insert(bytes.end(), record.begin(), record.end());
  }
  return bytes;
}

void strict_and_permissive_modes() {
  const auto directory_payload = from_hex(kGolden[1].hex);
  auto bad_add = from_hex(kGolden[10].hex);
  bad_add[19] = std::byte{'Z'};
  const auto good_add = from_hex(kGolden[10].hex);
  std::vector<std::byte> file;
  for (const auto &payload : {directory_payload, bad_add, good_add}) {
    const auto record = framed(payload);
    file.insert(file.end(), record.begin(), record.end());
  }

  replay::Replayer strict;
  CHECK(!strict.run(file, replay::ReplayMode::Strict));
  CHECK(strict.statistics().records_seen == 2);
  CHECK(strict.statistics().records_applied == 1);
  CHECK(strict.statistics().records_failed == 1);
  CHECK(strict.state().active_order_count() == 0);

  replay::Replayer permissive;
  CHECK(permissive.run(file, replay::ReplayMode::Permissive));
  CHECK(permissive.statistics().records_seen == 3);
  CHECK(permissive.statistics().records_applied == 2);
  CHECK(permissive.statistics().records_skipped == 1);
  CHECK(permissive.statistics().warning_counts.at(itch::ErrorCategory::InvalidEnum) == 1);
  CHECK(permissive.state().active_order_count() == 1);

  auto truncated = file;
  truncated.push_back(std::byte{0});
  replay::Replayer permissive_truncated;
  CHECK(!permissive_truncated.run(truncated, replay::ReplayMode::Permissive));
  CHECK(permissive_truncated.diagnostics().back().category ==
        itch::ErrorCategory::IncompleteEnvelope);
}

void full_synthetic_session_exact_state_statistics_and_digest() {
  const auto bytes = load_full_session_fixture();
  replay::Replayer replayer;
  CHECK(replayer.run(bytes, replay::ReplayMode::Strict));
  CHECK(replayer.statistics().records_seen == 28);
  CHECK(replayer.statistics().records_decoded == 28);
  CHECK(replayer.statistics().records_applied == 28);
  CHECK(replayer.statistics().message_type_counts.size() == 23);
  CHECK(replayer.statistics().message_type_counts.at('S') == 6);
  CHECK(replayer.state().session_phase() == replay::SessionPhase::MessagesEnded);
  CHECK(replayer.state().symbol_count() == 1);
  CHECK(replayer.state().active_order_count() == 1);
  CHECK(replayer.state().active_price_level_count() == 1);
  CHECK(replayer.state().order(4'702'394'921'427'289'928ULL)->remaining_shares == 80);
  CHECK(replayer.state().order(4'702'394'921'427'289'928ULL)->display_price == 1'234'400);
  CHECK(replayer.state().trade_count() == 4);
  const replay::VolumeStatistics expected_volumes{380, 245, 55, 1075, 30, 50};
  CHECK(replayer.state().volumes() == expected_volumes);
  CHECK(replayer.digest() == 16'906'756'921'747'084'480ULL);
}

void deterministic_text_json_and_digest_ten_runs() {
  const auto bytes = load_full_session_fixture();
  std::string expected_text;
  std::string expected_json;
  std::uint64_t expected_digest = 0;
  for (int run = 0; run < 10; ++run) {
    replay::Replayer replayer;
    CHECK(replayer.run(bytes, replay::ReplayMode::Strict));
    const auto text = replay::render_text(replayer, std::string{"AAPL"}, 10);
    const auto json = replay::render_json(replayer, std::string{"AAPL"}, 10);
    CHECK(json.starts_with('{') && json.ends_with("}\n"));
    CHECK(json.find("\"state_digest_fnv1a64\"") != std::string::npos);
    if (run == 0) {
      expected_text = text;
      expected_json = json;
      expected_digest = replayer.digest();
    } else {
      CHECK(text == expected_text);
      CHECK(json == expected_json);
      CHECK(replayer.digest() == expected_digest);
    }
  }

  replay::Replayer slice;
  CHECK(slice.run(bytes, replay::ReplayMode::Strict, 5));
  CHECK(slice.statistics().records_seen == 5);
  CHECK(slice.statistics().records_decoded == 5);
  CHECK(slice.statistics().records_applied == 5);
}

void book_event_v1_byte_exact_atomic_and_filtered() {
  CHECK(replay::valid_session_date("2026-08-24"));
  CHECK(replay::valid_session_date("2024-02-29"));
  CHECK(!replay::valid_session_date("2026-02-29"));
  CHECK(!replay::valid_session_date("2026-13-01"));
  CHECK(!replay::valid_session_date("20260824"));

  const auto bytes = load_full_session_fixture();
  const replay::BookEventV1Options options{"2026-08-24", 10, {}};
  replay::Replayer replayer;
  std::uint64_t output_records = 0;
  std::string actual = replay::render_book_event_v1_header(options, bytes.size()) + '\n';
  const replay::ApplyObserver observer = [&](const itch::Message &message, const itch::Frame &frame,
                                             const replay::FactualState &state) {
    if (replay::mutation_selected(state, options)) {
      actual += replay::render_book_event_v1(options, ++output_records, message, frame, state);
      actual += '\n';
    }
  };
  CHECK(replayer.run(bytes, replay::ReplayMode::Strict, std::nullopt, observer));
  actual += replay::render_book_event_v1_summary(replayer, output_records) + '\n';
  CHECK(output_records == 7);

  const auto golden_path =
      std::filesystem::path{LOB_SOURCE_DIR} / "tests" / "fixtures" / "book_event_v1_golden.ndjson";
  std::ifstream golden(golden_path, std::ios::binary);
  CHECK(golden.good());
  const std::string expected{std::istreambuf_iterator<char>{golden},
                             std::istreambuf_iterator<char>{}};
  CHECK(actual == expected);
  CHECK(actual.find("\"message_type\":\"U\"") != std::string::npos);
  CHECK(actual.find("\"display_price4\":1234600,\"execution_price4\":1234550") !=
        std::string::npos);

  const replay::BookEventV1Options filtered{"2026-08-24", 1, {"MSFT"}};
  replay::Replayer filtered_replayer;
  std::uint64_t filtered_records = 0;
  const replay::ApplyObserver filtered_observer = [&](const itch::Message &, const itch::Frame &,
                                                      const replay::FactualState &state) {
    filtered_records += replay::mutation_selected(state, filtered) ? 1U : 0U;
  };
  CHECK(filtered_replayer.run(bytes, replay::ReplayMode::Strict, std::nullopt, filtered_observer));
  CHECK(filtered_records == 0);
}

struct OracleOrder {
  itch::OrderReference reference{};
  itch::StockLocate locate{};
  std::string symbol;
  itch::FeedSide side{itch::FeedSide::Buy};
  itch::Price4 price{};
  itch::Shares shares{};
  std::optional<std::string> attribution;
  std::uint64_t sequence{};
};

class SlowOracle final {
public:
  std::vector<OracleOrder> orders;
  replay::VolumeStatistics volumes;
  std::vector<replay::TradeRecord> trades;
  std::uint64_t next_sequence{1};

  void apply(const itch::Message &message) {
    std::visit(
        [&](const auto &value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, itch::AddOrder>) {
            orders.push_back(OracleOrder{value.order_reference, value.header.stock_locate,
                                         value.stock.trimmed(), value.side, value.price,
                                         value.shares, std::nullopt, next_sequence++});
            volumes.displayed_add += value.shares;
          } else if constexpr (std::is_same_v<T, itch::AddOrderMpid>) {
            orders.push_back(OracleOrder{
                value.order_reference, value.header.stock_locate, value.stock.trimmed(), value.side,
                value.price, value.shares, value.attribution.trimmed(), next_sequence++});
            volumes.displayed_add += value.shares;
          } else if constexpr (std::is_same_v<T, itch::OrderExecuted> ||
                               std::is_same_v<T, itch::OrderExecutedWithPrice>) {
            const auto found = find(value.order_reference);
            const auto display_price = found->price;
            const bool printable = []<typename U>(const U &execution) {
              if constexpr (std::is_same_v<U, itch::OrderExecuted>) {
                return true;
              } else {
                return execution.printable == 'Y';
              }
            }(value);
            const itch::Price4 execution_price = [&]() {
              if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
                return display_price;
              } else {
                return value.execution_price;
              }
            }();
            trades.push_back(replay::TradeRecord{
                itch::message_type(message), value.match_number, found->locate, found->symbol,
                value.executed_shares, execution_price, printable, false});
            found->shares -= value.executed_shares;
            volumes.displayed_execute += value.executed_shares;
            (printable ? volumes.printable_trade : volumes.non_printable_trade) +=
                value.executed_shares;
            if (found->shares == 0) {
              orders.erase(found);
            }
          } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
            const auto found = find(value.order_reference);
            found->shares -= value.cancelled_shares;
            volumes.displayed_cancel += value.cancelled_shares;
            if (found->shares == 0) {
              orders.erase(found);
            }
          } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
            const auto found = find(value.order_reference);
            volumes.displayed_cancel += found->shares;
            orders.erase(found);
          } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
            const auto found = find(value.original_order_reference);
            OracleOrder replacement = *found;
            volumes.displayed_cancel += found->shares;
            volumes.displayed_add += value.shares;
            orders.erase(found);
            replacement.reference = value.new_order_reference;
            replacement.price = value.price;
            replacement.shares = value.shares;
            replacement.sequence = next_sequence++;
            orders.push_back(std::move(replacement));
          }
        },
        message);
  }

  [[nodiscard]] std::string canonical_book() const {
    auto sorted = orders;
    std::sort(sorted.begin(), sorted.end(), [](const OracleOrder &left, const OracleOrder &right) {
      if (left.symbol != right.symbol) {
        return left.symbol < right.symbol;
      }
      if (left.side != right.side) {
        return left.side == itch::FeedSide::Buy;
      }
      if (left.price != right.price) {
        return left.side == itch::FeedSide::Buy ? left.price > right.price
                                                : left.price < right.price;
      }
      return left.sequence < right.sequence;
    });
    std::ostringstream out;
    for (const auto &order : sorted) {
      out << order.reference << ':' << order.locate << ':' << order.symbol << ':'
          << static_cast<char>(order.side) << ':' << order.price << ':' << order.shares << ':'
          << order.sequence << ':';
      if (order.attribution.has_value()) {
        out << *order.attribution;
      }
      out << ';';
    }
    return out.str();
  }

private:
  std::vector<OracleOrder>::iterator find(const itch::OrderReference reference) {
    return std::find_if(orders.begin(), orders.end(), [reference](const OracleOrder &order) {
      return order.reference == reference;
    });
  }
};

std::string production_book_canonical(const replay::FactualState &state) {
  std::ostringstream out;
  for (const auto &order : state.active_orders()) {
    out << order.order_reference << ':' << order.stock_locate << ':' << order.symbol << ':'
        << static_cast<char>(order.side) << ':' << order.display_price << ':'
        << order.remaining_shares << ':' << order.fifo_sequence << ':';
    if (order.attribution.has_value()) {
      out << *order.attribution;
    }
    out << ';';
  }
  return out.str();
}

void differential_factual_oracle_100k_20_seeds() {
  constexpr std::array<std::uint64_t, 20> seeds{
      1,    7,     19,    42,    73,     101,    313,    997,     2003,    4099,
      8191, 16381, 32749, 65521, 131071, 262139, 524287, 1000003, 4294967, 9999991};
  constexpr std::array<std::string_view, 4> symbols{"AAPL", "MSFT", "NVDA", "TSLA"};
  for (const auto seed : seeds) {
    std::mt19937_64 rng(seed);
    replay::FactualState production;
    SlowOracle oracle;
    for (std::size_t index = 0; index < symbols.size(); ++index) {
      apply_ok(production,
               itch::Message{directory(static_cast<itch::StockLocate>(index + 1), symbols[index])});
    }
    itch::OrderReference next_reference = 1;
    itch::MatchNumber next_match = 1;
    for (std::size_t command_index = 0; command_index < 5'000; ++command_index) {
      itch::Message message;
      const auto roll = rng() % 100;
      if (oracle.orders.empty() || roll < 35) {
        const auto symbol_index = static_cast<std::size_t>(rng() % symbols.size());
        const auto side = (rng() & 1U) == 0 ? itch::FeedSide::Buy : itch::FeedSide::Sell;
        message = itch::Message{
            add_order(static_cast<itch::StockLocate>(symbol_index + 1), symbols[symbol_index],
                      next_reference++, side, static_cast<std::uint32_t>(1 + rng() % 500),
                      static_cast<itch::Price4>(900'000 + rng() % 200'001), command_index + 10)};
      } else {
        const auto order_index = static_cast<std::size_t>(rng() % oracle.orders.size());
        const auto selected = oracle.orders[order_index];
        if (roll < 55) {
          const auto quantity = static_cast<std::uint32_t>(1 + rng() % selected.shares);
          if ((rng() & 1U) == 0) {
            message =
                itch::Message{itch::OrderExecuted{header(selected.locate, command_index + 10),
                                                  selected.reference, quantity, next_match++}};
          } else {
            message = itch::Message{itch::OrderExecutedWithPrice{
                header(selected.locate, command_index + 10), selected.reference, quantity,
                next_match++, (rng() & 1U) == 0 ? 'Y' : 'N',
                static_cast<itch::Price4>(900'000 + rng() % 200'001)}};
          }
        } else if (roll < 75) {
          const auto quantity = static_cast<std::uint32_t>(1 + rng() % selected.shares);
          message = itch::Message{itch::OrderCancel{header(selected.locate, command_index + 10),
                                                    selected.reference, quantity}};
        } else if (roll < 90) {
          message = itch::Message{
              itch::OrderDelete{header(selected.locate, command_index + 10), selected.reference}};
        } else {
          message = itch::Message{
              itch::OrderReplace{header(selected.locate, command_index + 10), selected.reference,
                                 next_reference++, static_cast<std::uint32_t>(1 + rng() % 500),
                                 static_cast<itch::Price4>(900'000 + rng() % 200'001)}};
        }
      }
      apply_ok(production, message);
      oracle.apply(message);
      if ((command_index % 100) == 0 || command_index + 1 == 5'000) {
        CHECK(production_book_canonical(production) == oracle.canonical_book());
        CHECK(production.volumes() == oracle.volumes);
        CHECK(production.trades() == oracle.trades);
        std::string invariant_error;
        CHECK(production.check_invariants(&invariant_error));
      }
    }
  }
}

void invalid_property_sequences_preserve_state() {
  replay::FactualState state;
  apply_ok(state, itch::Message{directory(1, "AAPL")});
  apply_ok(state, itch::Message{add_order(1, "AAPL", 1, itch::FeedSide::Buy, 10, 100)});
  const std::array<itch::Message, 5> invalid{
      itch::Message{add_order(1, "AAPL", 1, itch::FeedSide::Sell, 5, 101)},
      itch::Message{itch::OrderExecuted{header(1), 99, 1, 5}},
      itch::Message{itch::OrderCancel{header(1), 1, 11}},
      itch::Message{itch::OrderReplace{header(1), 99, 2, 1, 100}},
      itch::Message{itch::BrokenTrade{header(1), 999}},
  };
  for (const auto &message : invalid) {
    const auto before = state.canonical_state();
    const auto result = state.apply(message);
    CHECK(!result.ok());
    CHECK(state.canonical_state() == before);
    std::string invariant_error;
    CHECK(state.check_invariants(&invariant_error));
  }
}

void write_fuzz_corpus(const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  for (const auto &fixture : kGolden) {
    const auto payload = from_hex(fixture.hex);
    const auto record = framed(payload);
    const auto path = directory / (std::string{"golden_"} + fixture.type);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(record.data()),
                 static_cast<std::streamsize>(record.size()));
  }
  const std::array<std::string_view, 5> malformed{"", "00", "000C5300", "00015A", "0000"};
  for (std::size_t index = 0; index < malformed.size(); ++index) {
    const auto bytes = from_hex(malformed[index]);
    std::ofstream output(directory / ("malformed_" + std::to_string(index)), std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
}

struct TestCase {
  std::string_view name;
  void (*function)();
};

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view{argv[1]} == "--write-fuzz-corpus") {
    write_fuzz_corpus(argv[2]);
    return 0;
  }
  if (argc == 3 && std::string_view{argv[1]} == "--write-full-session") {
    const auto bytes = load_full_session_fixture();
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good() ? 0 : 1;
  }
  const std::vector<TestCase> tests{
      {"byte_reader_endian_and_ascii", byte_reader_endian_and_ascii},
      {"golden_protocol_all_23_every_field", golden_protocol_all_23_every_field},
      {"every_truncated_prefix_and_wrong_length", every_truncated_prefix_and_wrong_length},
      {"malformed_enums_unknown_type_and_framing", malformed_enums_unknown_type_and_framing},
      {"add_execute_cancel_final_execute", add_execute_cancel_final_execute},
      {"execution_price_does_not_move_display_order", execution_price_does_not_move_display_order},
      {"delete_and_replace_inherit_identity_and_new_priority",
       delete_and_replace_inherit_identity_and_new_priority},
      {"fifo_levels_both_sides_and_multi_symbol_isolation",
       fifo_levels_both_sides_and_multi_symbol_isolation},
      {"semantic_errors_are_typed_and_atomic", semantic_errors_are_typed_and_atomic},
      {"trades_crosses_breaks_and_printability_do_not_change_depth",
       trades_crosses_breaks_and_printability_do_not_change_depth},
      {"valid_session_lifecycle_with_late_delete_and_break",
       valid_session_lifecycle_with_late_delete_and_break},
      {"uppercase_h_lowercase_h_and_o_distinctions", uppercase_h_lowercase_h_and_o_distinctions},
      {"strict_and_permissive_modes", strict_and_permissive_modes},
      {"full_synthetic_session_exact_state_statistics_and_digest",
       full_synthetic_session_exact_state_statistics_and_digest},
      {"deterministic_text_json_and_digest_ten_runs", deterministic_text_json_and_digest_ten_runs},
      {"book_event_v1_byte_exact_atomic_and_filtered",
       book_event_v1_byte_exact_atomic_and_filtered},
      {"differential_factual_oracle_100k_20_seeds", differential_factual_oracle_100k_20_seeds},
      {"invalid_property_sequences_preserve_state", invalid_property_sequences_preserve_state},
  };

  std::size_t passed = 0;
  for (const auto &test : tests) {
    try {
      test.function();
      ++passed;
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &failure) {
      std::cerr << "[FAIL] " << test.name << ": " << failure.what() << '\n';
      std::cerr << passed << '/' << tests.size() << " tests passed\n";
      return 1;
    }
  }
  std::cout << passed << '/' << tests.size()
            << " Round 2 tests passed; golden_types=23 randomized_mutations=100000 seeds=20\n";
  return 0;
}
