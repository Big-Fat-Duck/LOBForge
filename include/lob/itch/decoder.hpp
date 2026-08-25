#pragma once

#include "lob/itch/messages.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace lob::itch {

enum class ErrorCategory : std::uint8_t {
  IncompleteEnvelope,
  TruncatedFrame,
  EmptyPayload,
  UnknownMessageType,
  LengthMismatch,
  InvalidEnum,
  InvalidField,
  DuplicateOrderReference,
  UnknownOrderReference,
  UnknownStockLocate,
  StockLocateMismatch,
  SymbolMismatch,
  ZeroShares,
  QuantityExceedsRemaining,
  DuplicateReplacementReference,
  DirectoryConflict,
  DuplicateMatchNumber,
  UnknownMatchNumber,
  IllegalSessionTransition,
  AggregateOverflow,
  InvariantViolation
};

[[nodiscard]] std::string_view to_string(ErrorCategory category) noexcept;

struct ParseError {
  ErrorCategory category{ErrorCategory::InvalidField};
  std::size_t absolute_file_offset{};
  std::size_t record_index{};
  std::optional<char> message_type;
  std::optional<std::size_t> expected_length;
  std::optional<std::size_t> actual_length;
  std::string_view diagnostic;
  friend bool operator==(const ParseError &, const ParseError &) = default;
};

struct DecodeResult {
  std::optional<Message> message;
  std::optional<ParseError> error;
  [[nodiscard]] bool ok() const noexcept { return message.has_value(); }
};

[[nodiscard]] DecodeResult decode_message(std::span<const std::byte> payload,
                                          std::size_t absolute_payload_offset = 0,
                                          std::size_t record_index = 0) noexcept;

} // namespace lob::itch
