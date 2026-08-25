#include "lob/itch/decoder.hpp"

#include "lob/itch/byte_reader.hpp"

#include <string_view>

namespace lob::itch {
namespace {

constexpr FeedTimestamp kNanosecondsPerDay = 86'400'000'000'000ULL;
constexpr Price4 kMaximumPrice4 = 2'000'000'000U;

[[nodiscard]] bool one_of(const char value, const std::string_view allowed) noexcept {
  return allowed.find(value) != std::string_view::npos;
}

template <std::size_t N> [[nodiscard]] bool printable_ascii(const FixedAscii<N> &value) noexcept {
  for (const char character : value.raw) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || byte > 0x7EU) {
      return false;
    }
  }
  return true;
}

struct Decoder final {
  std::span<const std::byte> payload;
  std::size_t absolute_offset{};
  std::size_t record_index{};
  char type{};
  ByteReader reader;
  CommonHeader header;

  Decoder(std::span<const std::byte> bytes, const std::size_t offset, const std::size_t index)
      : payload(bytes), absolute_offset(offset), record_index(index), reader(bytes) {}

  [[nodiscard]] DecodeResult fail(const ErrorCategory category, const std::size_t field_offset,
                                  const std::string_view diagnostic) const noexcept {
    return {std::nullopt, ParseError{category, absolute_offset + field_offset, record_index,
                                     type == '\0' ? std::nullopt : std::optional<char>{type},
                                     std::nullopt, std::nullopt, diagnostic}};
  }

  [[nodiscard]] bool read_common() noexcept {
    return reader.read_char(type) && reader.read_be16(header.stock_locate) &&
           reader.read_be16(header.tracking_number) && reader.read_be48(header.timestamp);
  }

  [[nodiscard]] DecodeResult decode() noexcept {
    if (!read_common()) {
      return fail(ErrorCategory::LengthMismatch, reader.position(),
                  "payload ended while reading common header");
    }
    if (header.timestamp >= kNanosecondsPerDay) {
      return fail(ErrorCategory::InvalidField, 5, "timestamp is outside one trading day");
    }

    switch (type) {
    case 'S': {
      SystemEvent value{};
      value.header = header;
      if (!reader.read_char(value.event_code)) {
        return fail(ErrorCategory::LengthMismatch, 11, "missing system event code");
      }
      if (header.stock_locate != 0) {
        return fail(ErrorCategory::InvalidField, 1, "system event stock locate must be zero");
      }
      if (!one_of(value.event_code, "OSQMEC")) {
        return fail(ErrorCategory::InvalidEnum, 11, "invalid system event code");
      }
      return {Message{value}, std::nullopt};
    }
    case 'R': {
      StockDirectory value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) || !reader.read_char(value.market_category) ||
          !reader.read_char(value.financial_status) || !reader.read_be32(value.round_lot_size) ||
          !reader.read_char(value.round_lots_only) ||
          !reader.read_char(value.issue_classification) ||
          !reader.read_ascii(value.issue_sub_type) || !reader.read_char(value.authenticity) ||
          !reader.read_char(value.short_sale_threshold_indicator) ||
          !reader.read_char(value.ipo_flag) || !reader.read_char(value.luld_reference_price_tier) ||
          !reader.read_char(value.etp_flag) || !reader.read_be32(value.etp_leverage_factor) ||
          !reader.read_char(value.inverse_indicator)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "stock directory payload ended early");
      }
      if (!printable_ascii(value.stock) || !printable_ascii(value.issue_sub_type)) {
        return fail(ErrorCategory::InvalidField, 11, "stock directory contains non-ASCII text");
      }
      if (!one_of(value.market_category, "QGSNAPMZVF ")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid listing market category");
      }
      if (!one_of(value.financial_status, "DEQSGHJKCN ") || !one_of(value.round_lots_only, "YN") ||
          !one_of(value.authenticity, "PT") ||
          !one_of(value.short_sale_threshold_indicator, "YN ") || !one_of(value.ipo_flag, "YNZ ") ||
          !one_of(value.luld_reference_price_tier, "12 ") || !one_of(value.etp_flag, "YN ") ||
          !one_of(value.inverse_indicator, "YN")) {
        return fail(ErrorCategory::InvalidEnum, 20, "invalid stock directory enum field");
      }
      return {Message{value}, std::nullopt};
    }
    case 'H': {
      StockTradingAction value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) || !reader.read_char(value.trading_state) ||
          !reader.read_char(value.reserved) || !reader.read_ascii(value.reason)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "trading action payload ended early");
      }
      if (!printable_ascii(value.stock) || !printable_ascii(value.reason) ||
          value.reserved != ' ') {
        return fail(ErrorCategory::InvalidField, 11,
                    "invalid trading action text or reserved byte");
      }
      if (!one_of(value.trading_state, "HPQT")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid trading state");
      }
      return {Message{value}, std::nullopt};
    }
    case 'Y': {
      RegShoRestriction value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) || !reader.read_char(value.action)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "Reg SHO payload ended early");
      }
      if (!printable_ascii(value.stock)) {
        return fail(ErrorCategory::InvalidField, 11, "Reg SHO stock is not ASCII");
      }
      if (!one_of(value.action, "012")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid Reg SHO action");
      }
      return {Message{value}, std::nullopt};
    }
    case 'L': {
      MarketParticipantPosition value{};
      value.header = header;
      if (!reader.read_ascii(value.mpid) || !reader.read_ascii(value.stock) ||
          !reader.read_char(value.primary_market_maker) ||
          !reader.read_char(value.market_maker_mode) ||
          !reader.read_char(value.market_participant_state)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "market participant position payload ended early");
      }
      if (!printable_ascii(value.mpid) || !printable_ascii(value.stock)) {
        return fail(ErrorCategory::InvalidField, 11,
                    "participant position contains non-ASCII text");
      }
      if (!one_of(value.primary_market_maker, "YN") || !one_of(value.market_maker_mode, "NPSRL") ||
          !one_of(value.market_participant_state, "AEWSD")) {
        return fail(ErrorCategory::InvalidEnum, 23, "invalid participant position enum");
      }
      return {Message{value}, std::nullopt};
    }
    case 'V': {
      MwcbDeclineLevels value{};
      value.header = header;
      if (!reader.read_be64(value.level_1) || !reader.read_be64(value.level_2) ||
          !reader.read_be64(value.level_3)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "MWCB levels payload ended early");
      }
      if (header.stock_locate != 0) {
        return fail(ErrorCategory::InvalidField, 1, "MWCB levels stock locate must be zero");
      }
      return {Message{value}, std::nullopt};
    }
    case 'W': {
      MwcbStatus value{};
      value.header = header;
      if (!reader.read_char(value.breached_level)) {
        return fail(ErrorCategory::LengthMismatch, 11, "missing MWCB breached level");
      }
      if (header.stock_locate != 0) {
        return fail(ErrorCategory::InvalidField, 1, "MWCB status stock locate must be zero");
      }
      if (!one_of(value.breached_level, "123")) {
        return fail(ErrorCategory::InvalidEnum, 11, "invalid MWCB breached level");
      }
      return {Message{value}, std::nullopt};
    }
    case 'K': {
      IpoQuotingPeriodUpdate value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) || !reader.read_be32(value.ipo_quotation_release_time) ||
          !reader.read_char(value.ipo_quotation_release_qualifier) ||
          !reader.read_be32(value.ipo_price)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "IPO update payload ended early");
      }
      if (header.stock_locate != 0 || !printable_ascii(value.stock)) {
        return fail(ErrorCategory::InvalidField, 1, "invalid IPO update locate or stock");
      }
      if (!one_of(value.ipo_quotation_release_qualifier, "AC")) {
        return fail(ErrorCategory::InvalidEnum, 23, "invalid IPO release qualifier");
      }
      if (value.ipo_price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 24, "IPO price exceeds Price(4) maximum");
      }
      return {Message{value}, std::nullopt};
    }
    case 'J': {
      LuldAuctionCollar value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) ||
          !reader.read_be32(value.auction_collar_reference_price) ||
          !reader.read_be32(value.upper_auction_collar_price) ||
          !reader.read_be32(value.lower_auction_collar_price) ||
          !reader.read_be32(value.auction_collar_extension)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "LULD collar payload ended early");
      }
      if (!printable_ascii(value.stock) || value.auction_collar_reference_price > kMaximumPrice4 ||
          value.upper_auction_collar_price > kMaximumPrice4 ||
          value.lower_auction_collar_price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 11, "invalid LULD collar stock or price");
      }
      return {Message{value}, std::nullopt};
    }
    case 'h': {
      OperationalHalt value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) || !reader.read_char(value.market_code) ||
          !reader.read_char(value.operational_halt_action)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "operational halt payload ended early");
      }
      if (!printable_ascii(value.stock)) {
        return fail(ErrorCategory::InvalidField, 11, "operational halt stock is not ASCII");
      }
      if (!one_of(value.market_code, "QBX") || !one_of(value.operational_halt_action, "HT")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid operational halt enum");
      }
      return {Message{value}, std::nullopt};
    }
    case 'A': {
      AddOrder value{};
      value.header = header;
      char side = '\0';
      if (!reader.read_be64(value.order_reference) || !reader.read_char(side) ||
          !reader.read_be32(value.shares) || !reader.read_ascii(value.stock) ||
          !reader.read_be32(value.price)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "add order payload ended early");
      }
      if (!one_of(side, "BS")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid add-order side");
      }
      value.side = static_cast<FeedSide>(side);
      if (value.order_reference == 0 || value.shares == 0 || !printable_ascii(value.stock) ||
          value.price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 11,
                    "invalid add-order reference, shares, stock, or price");
      }
      return {Message{value}, std::nullopt};
    }
    case 'F': {
      AddOrderMpid value{};
      value.header = header;
      char side = '\0';
      if (!reader.read_be64(value.order_reference) || !reader.read_char(side) ||
          !reader.read_be32(value.shares) || !reader.read_ascii(value.stock) ||
          !reader.read_be32(value.price) || !reader.read_ascii(value.attribution)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "attributed add-order payload ended early");
      }
      if (!one_of(side, "BS")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid attributed add-order side");
      }
      value.side = static_cast<FeedSide>(side);
      if (value.order_reference == 0 || value.shares == 0 || !printable_ascii(value.stock) ||
          !printable_ascii(value.attribution) || value.price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 11,
                    "invalid attributed add-order reference, shares, text, or price");
      }
      return {Message{value}, std::nullopt};
    }
    case 'E': {
      OrderExecuted value{};
      value.header = header;
      if (!reader.read_be64(value.order_reference) || !reader.read_be32(value.executed_shares) ||
          !reader.read_be64(value.match_number)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "execution payload ended early");
      }
      if (value.order_reference == 0 || value.executed_shares == 0) {
        return fail(ErrorCategory::InvalidField, 11,
                    "execution reference and shares must be nonzero");
      }
      return {Message{value}, std::nullopt};
    }
    case 'C': {
      OrderExecutedWithPrice value{};
      value.header = header;
      if (!reader.read_be64(value.order_reference) || !reader.read_be32(value.executed_shares) ||
          !reader.read_be64(value.match_number) || !reader.read_char(value.printable) ||
          !reader.read_be32(value.execution_price)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "priced execution payload ended early");
      }
      if (!one_of(value.printable, "YN")) {
        return fail(ErrorCategory::InvalidEnum, 31, "invalid execution printable flag");
      }
      if (value.order_reference == 0 || value.executed_shares == 0 ||
          value.execution_price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 11,
                    "invalid priced execution reference, shares, or price");
      }
      return {Message{value}, std::nullopt};
    }
    case 'X': {
      OrderCancel value{};
      value.header = header;
      if (!reader.read_be64(value.order_reference) || !reader.read_be32(value.cancelled_shares)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(), "cancel payload ended early");
      }
      if (value.order_reference == 0 || value.cancelled_shares == 0) {
        return fail(ErrorCategory::InvalidField, 11, "cancel reference and shares must be nonzero");
      }
      return {Message{value}, std::nullopt};
    }
    case 'D': {
      OrderDelete value{};
      value.header = header;
      if (!reader.read_be64(value.order_reference)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(), "delete payload ended early");
      }
      if (value.order_reference == 0) {
        return fail(ErrorCategory::InvalidField, 11, "delete reference must be nonzero");
      }
      return {Message{value}, std::nullopt};
    }
    case 'U': {
      OrderReplace value{};
      value.header = header;
      if (!reader.read_be64(value.original_order_reference) ||
          !reader.read_be64(value.new_order_reference) || !reader.read_be32(value.shares) ||
          !reader.read_be32(value.price)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "replace payload ended early");
      }
      if (value.original_order_reference == 0 || value.new_order_reference == 0 ||
          value.original_order_reference == value.new_order_reference || value.shares == 0 ||
          value.price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 11,
                    "invalid replacement references, shares, or price");
      }
      return {Message{value}, std::nullopt};
    }
    case 'P': {
      NonCrossTrade value{};
      value.header = header;
      char side = '\0';
      if (!reader.read_be64(value.order_reference) || !reader.read_char(side) ||
          !reader.read_be32(value.shares) || !reader.read_ascii(value.stock) ||
          !reader.read_be32(value.price) || !reader.read_be64(value.match_number)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "non-cross trade payload ended early");
      }
      if (!one_of(side, "BS")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid non-cross trade side");
      }
      value.side = static_cast<FeedSide>(side);
      if (value.shares == 0 || !printable_ascii(value.stock) || value.price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 20,
                    "invalid non-cross trade shares, stock, or price");
      }
      return {Message{value}, std::nullopt};
    }
    case 'Q': {
      CrossTrade value{};
      value.header = header;
      if (!reader.read_be64(value.shares) || !reader.read_ascii(value.stock) ||
          !reader.read_be32(value.cross_price) || !reader.read_be64(value.match_number) ||
          !reader.read_char(value.cross_type)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "cross trade payload ended early");
      }
      if (!printable_ascii(value.stock) || value.cross_price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 19, "invalid cross-trade stock or price");
      }
      if (!one_of(value.cross_type, "OCH")) {
        return fail(ErrorCategory::InvalidEnum, 39, "invalid cross type");
      }
      return {Message{value}, std::nullopt};
    }
    case 'B': {
      BrokenTrade value{};
      value.header = header;
      if (!reader.read_be64(value.match_number)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(),
                    "broken-trade payload ended early");
      }
      return {Message{value}, std::nullopt};
    }
    case 'I': {
      Noii value{};
      value.header = header;
      if (!reader.read_be64(value.paired_shares) || !reader.read_be64(value.imbalance_shares) ||
          !reader.read_char(value.imbalance_direction) || !reader.read_ascii(value.stock) ||
          !reader.read_be32(value.far_price) || !reader.read_be32(value.near_price) ||
          !reader.read_be32(value.current_reference_price) || !reader.read_char(value.cross_type) ||
          !reader.read_char(value.price_variation_indicator)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(), "NOII payload ended early");
      }
      if (!printable_ascii(value.stock) || value.far_price > kMaximumPrice4 ||
          value.near_price > kMaximumPrice4 || value.current_reference_price > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 28, "invalid NOII stock or price");
      }
      if (!one_of(value.imbalance_direction, "BSNOP") || !one_of(value.cross_type, "OCHA") ||
          !one_of(value.price_variation_indicator, "L123456789ABC ")) {
        return fail(ErrorCategory::InvalidEnum, 27, "invalid NOII enum field");
      }
      return {Message{value}, std::nullopt};
    }
    case 'N': {
      RetailPriceImprovement value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) || !reader.read_char(value.interest_flag)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(), "RPII payload ended early");
      }
      if (!printable_ascii(value.stock)) {
        return fail(ErrorCategory::InvalidField, 11, "RPII stock is not ASCII");
      }
      if (!one_of(value.interest_flag, "BSAN")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid RPII interest flag");
      }
      return {Message{value}, std::nullopt};
    }
    case 'O': {
      DlcrPriceDiscovery value{};
      value.header = header;
      if (!reader.read_ascii(value.stock) || !reader.read_char(value.open_eligibility_status) ||
          !reader.read_be32(value.minimum_allowable_price) ||
          !reader.read_be32(value.maximum_allowable_price) ||
          !reader.read_be32(value.near_execution_price) ||
          !reader.read_be64(value.near_execution_time) ||
          !reader.read_be32(value.lower_price_range_collar) ||
          !reader.read_be32(value.upper_price_range_collar)) {
        return fail(ErrorCategory::LengthMismatch, reader.position(), "DLCR payload ended early");
      }
      if (!printable_ascii(value.stock)) {
        return fail(ErrorCategory::InvalidField, 11, "DLCR stock is not ASCII");
      }
      if (!one_of(value.open_eligibility_status, "YN")) {
        return fail(ErrorCategory::InvalidEnum, 19, "invalid DLCR eligibility status");
      }
      if (value.minimum_allowable_price > kMaximumPrice4 ||
          value.maximum_allowable_price > kMaximumPrice4 ||
          value.near_execution_price > kMaximumPrice4 ||
          value.lower_price_range_collar > kMaximumPrice4 ||
          value.upper_price_range_collar > kMaximumPrice4) {
        return fail(ErrorCategory::InvalidField, 20, "DLCR Price(4) field exceeds maximum");
      }
      return {Message{value}, std::nullopt};
    }
    default:
      return fail(ErrorCategory::UnknownMessageType, 0, "unknown ITCH application message type");
    }
  }
};

} // namespace

std::string_view to_string(const ErrorCategory category) noexcept {
  switch (category) {
  case ErrorCategory::IncompleteEnvelope:
    return "incomplete_envelope";
  case ErrorCategory::TruncatedFrame:
    return "truncated_frame";
  case ErrorCategory::EmptyPayload:
    return "empty_payload";
  case ErrorCategory::UnknownMessageType:
    return "unknown_message_type";
  case ErrorCategory::LengthMismatch:
    return "length_mismatch";
  case ErrorCategory::InvalidEnum:
    return "invalid_enum";
  case ErrorCategory::InvalidField:
    return "invalid_field";
  case ErrorCategory::DuplicateOrderReference:
    return "duplicate_order_reference";
  case ErrorCategory::UnknownOrderReference:
    return "unknown_order_reference";
  case ErrorCategory::UnknownStockLocate:
    return "unknown_stock_locate";
  case ErrorCategory::StockLocateMismatch:
    return "stock_locate_mismatch";
  case ErrorCategory::SymbolMismatch:
    return "symbol_mismatch";
  case ErrorCategory::ZeroShares:
    return "zero_shares";
  case ErrorCategory::QuantityExceedsRemaining:
    return "quantity_exceeds_remaining";
  case ErrorCategory::DuplicateReplacementReference:
    return "duplicate_replacement_reference";
  case ErrorCategory::DirectoryConflict:
    return "directory_conflict";
  case ErrorCategory::DuplicateMatchNumber:
    return "duplicate_match_number";
  case ErrorCategory::UnknownMatchNumber:
    return "unknown_match_number";
  case ErrorCategory::IllegalSessionTransition:
    return "illegal_session_transition";
  case ErrorCategory::AggregateOverflow:
    return "aggregate_overflow";
  case ErrorCategory::InvariantViolation:
    return "invariant_violation";
  }
  return "unknown_error";
}

DecodeResult decode_message(const std::span<const std::byte> payload,
                            const std::size_t absolute_payload_offset,
                            const std::size_t record_index) noexcept {
  if (payload.empty()) {
    return {std::nullopt,
            ParseError{ErrorCategory::EmptyPayload, absolute_payload_offset, record_index,
                       std::nullopt, std::nullopt, 0, "record payload is empty"}};
  }
  const char type = static_cast<char>(std::to_integer<unsigned char>(payload.front()));
  const auto expected = expected_payload_length(type);
  if (!expected.has_value()) {
    return {std::nullopt, ParseError{ErrorCategory::UnknownMessageType, absolute_payload_offset,
                                     record_index, type, std::nullopt, payload.size(),
                                     "unknown ITCH application message type"}};
  }
  if (payload.size() != *expected) {
    return {std::nullopt,
            ParseError{ErrorCategory::LengthMismatch, absolute_payload_offset, record_index, type,
                       *expected, payload.size(), "known message has wrong payload length"}};
  }
  return Decoder{payload, absolute_payload_offset, record_index}.decode();
}

} // namespace lob::itch
