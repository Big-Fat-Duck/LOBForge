#include "lob/replay/factual_book.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_set>

namespace lob::replay {
namespace {

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();

[[nodiscard]] bool can_add(const std::uint64_t current, const std::uint64_t quantity) noexcept {
  return quantity <= kMax - current;
}

[[nodiscard]] std::string reference_key(const itch::Message &message) {
  const char type = itch::message_type(message);
  std::string key(1, type);
  std::visit(
      [&key](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, itch::StockTradingAction> ||
                      std::is_same_v<T, itch::RegShoRestriction> ||
                      std::is_same_v<T, itch::IpoQuotingPeriodUpdate> ||
                      std::is_same_v<T, itch::LuldAuctionCollar> ||
                      std::is_same_v<T, itch::RetailPriceImprovement> ||
                      std::is_same_v<T, itch::DlcrPriceDiscovery> ||
                      std::is_same_v<T, itch::Noii>) {
          key += '|' + value.stock.trimmed();
        } else if constexpr (std::is_same_v<T, itch::MarketParticipantPosition>) {
          key += '|' + value.stock.trimmed() + '|' + value.mpid.trimmed();
        } else if constexpr (std::is_same_v<T, itch::OperationalHalt>) {
          key += '|' + value.stock.trimmed() + '|' + std::string(1, value.market_code);
        }
      },
      message);
  return key;
}

} // namespace

std::string_view to_string(const SessionPhase phase) noexcept {
  switch (phase) {
  case SessionPhase::BeforeMessages:
    return "before_messages";
  case SessionPhase::MessagesStarted:
    return "messages_started";
  case SessionPhase::SystemHours:
    return "system_hours";
  case SessionPhase::MarketHours:
    return "market_hours";
  case SessionPhase::MarketHoursEnded:
    return "market_hours_ended";
  case SessionPhase::SystemHoursEnded:
    return "system_hours_ended";
  case SessionPhase::MessagesEnded:
    return "messages_ended";
  }
  return "unknown";
}

itch::ParseError FactualState::error(const itch::ErrorCategory category, const char type,
                                     const std::size_t absolute_file_offset,
                                     const std::size_t record_index,
                                     const std::string_view diagnostic) const {
  return itch::ParseError{category,     absolute_file_offset, record_index, type,
                          std::nullopt, std::nullopt,         diagnostic};
}

bool FactualState::add_volume(std::uint64_t &target, const std::uint64_t quantity) const noexcept {
  if (!can_add(target, quantity)) {
    return false;
  }
  target += quantity;
  return true;
}

ApplyResult FactualState::apply(const itch::Message &message,
                                const std::size_t absolute_file_offset,
                                const std::size_t record_index) {
  if (capture_book_mutations_) {
    last_book_mutation_.reset();
  }
  auto result = std::visit(
      [&](const auto &value) -> ApplyResult {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, itch::SystemEvent>) {
          return apply_system_event(value, absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::StockDirectory>) {
          return apply_directory(value, absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::AddOrder>) {
          return apply_add(value.header, value.order_reference, value.side, value.shares,
                           value.stock, value.price, std::nullopt, 'A', absolute_file_offset,
                           record_index);
        } else if constexpr (std::is_same_v<T, itch::AddOrderMpid>) {
          return apply_add(value.header, value.order_reference, value.side, value.shares,
                           value.stock, value.price, value.attribution.trimmed(), 'F',
                           absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
          return apply_execution(value.header, value.order_reference, value.executed_shares,
                                 value.match_number, std::nullopt, true, 'E', absolute_file_offset,
                                 record_index);
        } else if constexpr (std::is_same_v<T, itch::OrderExecutedWithPrice>) {
          return apply_execution(value.header, value.order_reference, value.executed_shares,
                                 value.match_number, value.execution_price, value.printable == 'Y',
                                 'C', absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
          return apply_cancel(value, absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
          return apply_delete(value, absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
          return apply_replace(value, absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::NonCrossTrade>) {
          return apply_non_cross_trade(value, absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::CrossTrade>) {
          return apply_cross_trade(value, absolute_file_offset, record_index);
        } else if constexpr (std::is_same_v<T, itch::BrokenTrade>) {
          return apply_broken_trade(value, absolute_file_offset, record_index);
        } else {
          return apply_reference(message, absolute_file_offset, record_index);
        }
      },
      message);
  if (result.ok()) {
    ++applied_messages_;
#ifndef NDEBUG
    std::string invariant_error;
    assert(check_invariants(&invariant_error));
#endif
  }
  return result;
}

ApplyResult FactualState::apply_system_event(const itch::SystemEvent &message,
                                             const std::size_t absolute_file_offset,
                                             const std::size_t record_index) {
  SessionPhase next = session_phase_;
  bool valid = false;
  switch (message.event_code) {
  case 'O':
    valid = session_phase_ == SessionPhase::BeforeMessages;
    next = SessionPhase::MessagesStarted;
    break;
  case 'S':
    valid = session_phase_ == SessionPhase::MessagesStarted;
    next = SessionPhase::SystemHours;
    break;
  case 'Q':
    valid = session_phase_ == SessionPhase::SystemHours;
    next = SessionPhase::MarketHours;
    break;
  case 'M':
    valid = session_phase_ == SessionPhase::MarketHours;
    next = SessionPhase::MarketHoursEnded;
    break;
  case 'E':
    valid = session_phase_ == SessionPhase::MarketHoursEnded;
    next = SessionPhase::SystemHoursEnded;
    break;
  case 'C':
    valid = session_phase_ == SessionPhase::SystemHoursEnded;
    next = SessionPhase::MessagesEnded;
    break;
  default:
    break;
  }
  if (!valid) {
    return {error(itch::ErrorCategory::IllegalSessionTransition, 'S', absolute_file_offset,
                  record_index, "system event is illegal in the current session phase")};
  }
  session_phase_ = next;
  reference_state_["S"] = itch::Message{message};
  return {};
}

ApplyResult FactualState::apply_directory(const itch::StockDirectory &message,
                                          const std::size_t absolute_file_offset,
                                          const std::size_t record_index) {
  const std::string symbol = message.stock.trimmed();
  if (message.header.stock_locate == 0 || symbol.empty()) {
    return {error(itch::ErrorCategory::InvalidField, 'R', absolute_file_offset, record_index,
                  "stock directory requires nonzero locate and nonempty symbol")};
  }
  const auto locate = directories_.find(message.header.stock_locate);
  if (locate != directories_.end() && locate->second.stock != message.stock) {
    return {error(itch::ErrorCategory::DirectoryConflict, 'R', absolute_file_offset, record_index,
                  "stock locate is already assigned to another symbol")};
  }
  const auto existing_symbol = symbol_locates_.find(symbol);
  if (existing_symbol != symbol_locates_.end() &&
      existing_symbol->second != message.header.stock_locate) {
    return {error(itch::ErrorCategory::DirectoryConflict, 'R', absolute_file_offset, record_index,
                  "symbol is already assigned to another stock locate")};
  }
  directories_.insert_or_assign(message.header.stock_locate, message);
  symbol_locates_.insert_or_assign(symbol, message.header.stock_locate);
  return {};
}

std::optional<itch::ParseError>
FactualState::validate_locate_symbol(const itch::CommonHeader &header, const itch::Stock &stock,
                                     const char type, const std::size_t absolute_file_offset,
                                     const std::size_t record_index) const {
  const auto directory = directories_.find(header.stock_locate);
  if (directory == directories_.end()) {
    return error(itch::ErrorCategory::UnknownStockLocate, type, absolute_file_offset, record_index,
                 "stock locate has no day-local Stock Directory entry");
  }
  if (directory->second.stock != stock) {
    return error(itch::ErrorCategory::SymbolMismatch, type, absolute_file_offset, record_index,
                 "message symbol disagrees with Stock Directory mapping");
  }
  return std::nullopt;
}

ApplyResult FactualState::apply_add(const itch::CommonHeader &header,
                                    const itch::OrderReference order_reference,
                                    const itch::FeedSide side, const std::uint32_t shares,
                                    const itch::Stock &stock, const itch::Price4 price,
                                    std::optional<std::string> attribution, const char source_type,
                                    const std::size_t absolute_file_offset,
                                    const std::size_t record_index) {
  if (const auto locate_error =
          validate_locate_symbol(header, stock, source_type, absolute_file_offset, record_index)) {
    return {*locate_error};
  }
  if (shares == 0) {
    return {error(itch::ErrorCategory::ZeroShares, source_type, absolute_file_offset, record_index,
                  "displayed add shares must be positive")};
  }
  if (order_index_.contains(order_reference)) {
    return {error(itch::ErrorCategory::DuplicateOrderReference, source_type, absolute_file_offset,
                  record_index, "active order reference already exists")};
  }
  const std::string symbol = stock.trimmed();
  const itch::Shares current = [&]() {
    const auto book = books_.find(symbol);
    if (book == books_.end()) {
      return itch::Shares{0};
    }
    if (side == itch::FeedSide::Buy) {
      const auto level = book->second.bids.find(price);
      return level == book->second.bids.end() ? itch::Shares{0} : level->second.aggregate_shares;
    }
    const auto level = book->second.asks.find(price);
    return level == book->second.asks.end() ? itch::Shares{0} : level->second.aggregate_shares;
  }();
  if (!can_add(current, shares) || !can_add(volumes_.displayed_add, shares) ||
      next_fifo_sequence_ == kMax) {
    return {error(itch::ErrorCategory::AggregateOverflow, source_type, absolute_file_offset,
                  record_index, "displayed add would overflow aggregate or FIFO sequence")};
  }

  auto &book = books_[symbol];
  RestingOrder resting{order_reference,        header.stock_locate,  symbol, side, price, shares,
                       std::move(attribution), next_fifo_sequence_++};
  if (side == itch::FeedSide::Buy) {
    auto &level = book.bids[price];
    level.orders.push_back(std::move(resting));
    auto order = std::prev(level.orders.end());
    level.aggregate_shares += shares;
    order_index_.emplace(order_reference, IndexEntry{side, symbol, price, order});
  } else {
    auto &level = book.asks[price];
    level.orders.push_back(std::move(resting));
    auto order = std::prev(level.orders.end());
    level.aggregate_shares += shares;
    order_index_.emplace(order_reference, IndexEntry{side, symbol, price, order});
  }
  volumes_.displayed_add += shares;
  if (capture_book_mutations_) {
    last_book_mutation_ = BookMutation{source_type,     header.stock_locate, symbol, side,
                                       order_reference, std::nullopt,        shares, price,
                                       std::nullopt,    std::nullopt};
  }
  return {};
}

ApplyResult FactualState::apply_execution(
    const itch::CommonHeader &header, const itch::OrderReference order_reference,
    const std::uint32_t shares, const itch::MatchNumber match_number,
    const std::optional<itch::Price4> execution_price, const bool printable, const char source_type,
    const std::size_t absolute_file_offset, const std::size_t record_index) {
  const auto indexed = order_index_.find(order_reference);
  if (indexed == order_index_.end()) {
    return {error(itch::ErrorCategory::UnknownOrderReference, source_type, absolute_file_offset,
                  record_index, "execution references no active displayed order")};
  }
  if (indexed->second.order->stock_locate != header.stock_locate) {
    return {error(itch::ErrorCategory::StockLocateMismatch, source_type, absolute_file_offset,
                  record_index, "execution stock locate disagrees with active order")};
  }
  if (shares == 0) {
    return {error(itch::ErrorCategory::ZeroShares, source_type, absolute_file_offset, record_index,
                  "executed shares must be positive")};
  }
  if (shares > indexed->second.order->remaining_shares) {
    return {error(itch::ErrorCategory::QuantityExceedsRemaining, source_type, absolute_file_offset,
                  record_index, "executed shares exceed displayed remainder")};
  }
  if (trade_index_.contains(match_number)) {
    return {error(itch::ErrorCategory::DuplicateMatchNumber, source_type, absolute_file_offset,
                  record_index, "match number already exists in trade ledger")};
  }
  auto &trade_volume = printable ? volumes_.printable_trade : volumes_.non_printable_trade;
  if (!can_add(volumes_.displayed_execute, shares) || !can_add(trade_volume, shares)) {
    return {error(itch::ErrorCategory::AggregateOverflow, source_type, absolute_file_offset,
                  record_index, "execution volume counter would overflow")};
  }

  std::optional<BookMutation> mutation;
  if (capture_book_mutations_) {
    const auto &order = *indexed->second.order;
    mutation = BookMutation{source_type,     order.stock_locate, order.symbol, order.side,
                            order_reference, std::nullopt,       shares,       order.display_price,
                            execution_price, match_number};
  }
  const auto &order = *indexed->second.order;
  trades_.push_back(TradeRecord{source_type, match_number, order.stock_locate, order.symbol, shares,
                                execution_price.value_or(order.display_price), printable, false});
  trade_index_.emplace(match_number, trades_.size() - 1);
  volumes_.displayed_execute += shares;
  trade_volume += shares;
  indexed->second.order->remaining_shares -= shares;
  if (indexed->second.side == itch::FeedSide::Buy) {
    books_.at(indexed->second.symbol).bids.at(indexed->second.price).aggregate_shares -= shares;
  } else {
    books_.at(indexed->second.symbol).asks.at(indexed->second.price).aggregate_shares -= shares;
  }
  if (indexed->second.order->remaining_shares == 0) {
    erase_order(indexed);
  }
  if (mutation.has_value()) {
    last_book_mutation_ = std::move(mutation);
  }
  return {};
}

ApplyResult FactualState::apply_cancel(const itch::OrderCancel &message,
                                       const std::size_t absolute_file_offset,
                                       const std::size_t record_index) {
  const auto indexed = order_index_.find(message.order_reference);
  if (indexed == order_index_.end()) {
    return {error(itch::ErrorCategory::UnknownOrderReference, 'X', absolute_file_offset,
                  record_index, "cancel references no active displayed order")};
  }
  if (indexed->second.order->stock_locate != message.header.stock_locate) {
    return {error(itch::ErrorCategory::StockLocateMismatch, 'X', absolute_file_offset, record_index,
                  "cancel stock locate disagrees with active order")};
  }
  if (message.cancelled_shares == 0) {
    return {error(itch::ErrorCategory::ZeroShares, 'X', absolute_file_offset, record_index,
                  "cancelled shares must be positive")};
  }
  if (message.cancelled_shares > indexed->second.order->remaining_shares) {
    return {error(itch::ErrorCategory::QuantityExceedsRemaining, 'X', absolute_file_offset,
                  record_index, "cancelled shares exceed displayed remainder")};
  }
  if (!can_add(volumes_.displayed_cancel, message.cancelled_shares)) {
    return {error(itch::ErrorCategory::AggregateOverflow, 'X', absolute_file_offset, record_index,
                  "cancel volume counter would overflow")};
  }
  std::optional<BookMutation> mutation;
  if (capture_book_mutations_) {
    const auto &order = *indexed->second.order;
    mutation = BookMutation{'X',
                            order.stock_locate,
                            order.symbol,
                            order.side,
                            message.order_reference,
                            std::nullopt,
                            message.cancelled_shares,
                            order.display_price,
                            std::nullopt,
                            std::nullopt};
  }
  indexed->second.order->remaining_shares -= message.cancelled_shares;
  if (indexed->second.side == itch::FeedSide::Buy) {
    books_.at(indexed->second.symbol).bids.at(indexed->second.price).aggregate_shares -=
        message.cancelled_shares;
  } else {
    books_.at(indexed->second.symbol).asks.at(indexed->second.price).aggregate_shares -=
        message.cancelled_shares;
  }
  volumes_.displayed_cancel += message.cancelled_shares;
  if (indexed->second.order->remaining_shares == 0) {
    erase_order(indexed);
  }
  if (mutation.has_value()) {
    last_book_mutation_ = std::move(mutation);
  }
  return {};
}

ApplyResult FactualState::apply_delete(const itch::OrderDelete &message,
                                       const std::size_t absolute_file_offset,
                                       const std::size_t record_index) {
  const auto indexed = order_index_.find(message.order_reference);
  if (indexed == order_index_.end()) {
    return {error(itch::ErrorCategory::UnknownOrderReference, 'D', absolute_file_offset,
                  record_index, "delete references no active displayed order")};
  }
  if (indexed->second.order->stock_locate != message.header.stock_locate) {
    return {error(itch::ErrorCategory::StockLocateMismatch, 'D', absolute_file_offset, record_index,
                  "delete stock locate disagrees with active order")};
  }
  const itch::Shares remaining = indexed->second.order->remaining_shares;
  if (!can_add(volumes_.displayed_cancel, remaining)) {
    return {error(itch::ErrorCategory::AggregateOverflow, 'D', absolute_file_offset, record_index,
                  "delete volume counter would overflow")};
  }
  std::optional<BookMutation> mutation;
  if (capture_book_mutations_) {
    const auto &order = *indexed->second.order;
    mutation = BookMutation{'D',         order.stock_locate,      order.symbol,
                            order.side,  message.order_reference, std::nullopt,
                            remaining,   order.display_price,     std::nullopt,
                            std::nullopt};
  }
  volumes_.displayed_cancel += remaining;
  erase_order(indexed);
  if (mutation.has_value()) {
    last_book_mutation_ = std::move(mutation);
  }
  return {};
}

ApplyResult FactualState::apply_replace(const itch::OrderReplace &message,
                                        const std::size_t absolute_file_offset,
                                        const std::size_t record_index) {
  const auto indexed = order_index_.find(message.original_order_reference);
  if (indexed == order_index_.end()) {
    return {error(itch::ErrorCategory::UnknownOrderReference, 'U', absolute_file_offset,
                  record_index, "replace references no active original order")};
  }
  if (indexed->second.order->stock_locate != message.header.stock_locate) {
    return {error(itch::ErrorCategory::StockLocateMismatch, 'U', absolute_file_offset, record_index,
                  "replace stock locate disagrees with original order")};
  }
  if (order_index_.contains(message.new_order_reference)) {
    return {error(itch::ErrorCategory::DuplicateReplacementReference, 'U', absolute_file_offset,
                  record_index, "replacement reference is already active")};
  }
  if (message.shares == 0) {
    return {error(itch::ErrorCategory::ZeroShares, 'U', absolute_file_offset, record_index,
                  "replacement shares must be positive")};
  }
  const RestingOrder original = *indexed->second.order;
  itch::Shares target_aggregate = 0;
  const auto &book = books_.at(original.symbol);
  if (original.side == itch::FeedSide::Buy) {
    const auto level = book.bids.find(message.price);
    if (level != book.bids.end()) {
      target_aggregate = level->second.aggregate_shares;
    }
  } else {
    const auto level = book.asks.find(message.price);
    if (level != book.asks.end()) {
      target_aggregate = level->second.aggregate_shares;
    }
  }
  if (message.price == original.display_price) {
    target_aggregate -= original.remaining_shares;
  }
  if (!can_add(target_aggregate, message.shares) ||
      !can_add(volumes_.displayed_cancel, original.remaining_shares) ||
      !can_add(volumes_.displayed_add, message.shares) || next_fifo_sequence_ == kMax) {
    return {error(itch::ErrorCategory::AggregateOverflow, 'U', absolute_file_offset, record_index,
                  "replacement would overflow aggregate, volume, or FIFO sequence")};
  }

  erase_order(indexed);
  RestingOrder replacement{message.new_order_reference,
                           original.stock_locate,
                           original.symbol,
                           original.side,
                           message.price,
                           message.shares,
                           original.attribution,
                           next_fifo_sequence_++};
  auto &mutable_book = books_[original.symbol];
  if (original.side == itch::FeedSide::Buy) {
    auto &level = mutable_book.bids[message.price];
    level.orders.push_back(std::move(replacement));
    auto order = std::prev(level.orders.end());
    level.aggregate_shares += message.shares;
    order_index_.emplace(message.new_order_reference,
                         IndexEntry{original.side, original.symbol, message.price, order});
  } else {
    auto &level = mutable_book.asks[message.price];
    level.orders.push_back(std::move(replacement));
    auto order = std::prev(level.orders.end());
    level.aggregate_shares += message.shares;
    order_index_.emplace(message.new_order_reference,
                         IndexEntry{original.side, original.symbol, message.price, order});
  }
  volumes_.displayed_cancel += original.remaining_shares;
  volumes_.displayed_add += message.shares;
  if (capture_book_mutations_) {
    last_book_mutation_ = BookMutation{'U',
                                       original.stock_locate,
                                       original.symbol,
                                       original.side,
                                       message.original_order_reference,
                                       message.new_order_reference,
                                       message.shares,
                                       message.price,
                                       std::nullopt,
                                       std::nullopt};
  }
  return {};
}

ApplyResult FactualState::apply_non_cross_trade(const itch::NonCrossTrade &message,
                                                const std::size_t absolute_file_offset,
                                                const std::size_t record_index) {
  if (const auto locate_error = validate_locate_symbol(message.header, message.stock, 'P',
                                                       absolute_file_offset, record_index)) {
    return {*locate_error};
  }
  if (trade_index_.contains(message.match_number)) {
    return {error(itch::ErrorCategory::DuplicateMatchNumber, 'P', absolute_file_offset,
                  record_index, "match number already exists in trade ledger")};
  }
  if (!can_add(volumes_.printable_trade, message.shares)) {
    return {error(itch::ErrorCategory::AggregateOverflow, 'P', absolute_file_offset, record_index,
                  "printable trade volume would overflow")};
  }
  trades_.push_back(TradeRecord{'P', message.match_number, message.header.stock_locate,
                                message.stock.trimmed(), message.shares, message.price, true,
                                false});
  trade_index_.emplace(message.match_number, trades_.size() - 1);
  volumes_.printable_trade += message.shares;
  return {};
}

ApplyResult FactualState::apply_cross_trade(const itch::CrossTrade &message,
                                            const std::size_t absolute_file_offset,
                                            const std::size_t record_index) {
  if (const auto locate_error = validate_locate_symbol(message.header, message.stock, 'Q',
                                                       absolute_file_offset, record_index)) {
    return {*locate_error};
  }
  if (trade_index_.contains(message.match_number)) {
    return {error(itch::ErrorCategory::DuplicateMatchNumber, 'Q', absolute_file_offset,
                  record_index, "match number already exists in trade ledger")};
  }
  if (!can_add(volumes_.printable_trade, message.shares)) {
    return {error(itch::ErrorCategory::AggregateOverflow, 'Q', absolute_file_offset, record_index,
                  "cross trade volume would overflow")};
  }
  trades_.push_back(TradeRecord{'Q', message.match_number, message.header.stock_locate,
                                message.stock.trimmed(), message.shares, message.cross_price, true,
                                false});
  trade_index_.emplace(message.match_number, trades_.size() - 1);
  volumes_.printable_trade += message.shares;
  return {};
}

ApplyResult FactualState::apply_broken_trade(const itch::BrokenTrade &message,
                                             const std::size_t absolute_file_offset,
                                             const std::size_t record_index) {
  const auto indexed = trade_index_.find(message.match_number);
  if (indexed == trade_index_.end() || trades_[indexed->second].broken) {
    return {error(itch::ErrorCategory::UnknownMatchNumber, 'B', absolute_file_offset, record_index,
                  "broken trade references no unbroken ledger execution")};
  }
  auto &trade = trades_[indexed->second];
  if (trade.stock_locate != message.header.stock_locate) {
    return {error(itch::ErrorCategory::StockLocateMismatch, 'B', absolute_file_offset, record_index,
                  "broken trade locate disagrees with ledger execution")};
  }
  if (!can_add(volumes_.broken_trade, trade.shares)) {
    return {error(itch::ErrorCategory::AggregateOverflow, 'B', absolute_file_offset, record_index,
                  "broken trade volume would overflow")};
  }
  trade.broken = true;
  volumes_.broken_trade += trade.shares;
  return {};
}

ApplyResult FactualState::apply_reference(const itch::Message &message,
                                          const std::size_t absolute_file_offset,
                                          const std::size_t record_index) {
  std::optional<itch::ParseError> validation;
  std::visit(
      [&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, itch::StockTradingAction> ||
                      std::is_same_v<T, itch::RegShoRestriction> ||
                      std::is_same_v<T, itch::MarketParticipantPosition> ||
                      std::is_same_v<T, itch::LuldAuctionCollar> ||
                      std::is_same_v<T, itch::OperationalHalt> || std::is_same_v<T, itch::Noii> ||
                      std::is_same_v<T, itch::RetailPriceImprovement> ||
                      std::is_same_v<T, itch::DlcrPriceDiscovery>) {
          validation =
              validate_locate_symbol(value.header, value.stock, itch::message_type(message),
                                     absolute_file_offset, record_index);
        }
      },
      message);
  if (validation.has_value()) {
    return {*validation};
  }
  reference_state_.insert_or_assign(reference_key(message), message);
  return {};
}

void FactualState::erase_order(
    const std::unordered_map<itch::OrderReference, IndexEntry>::iterator indexed) {
  auto book = books_.find(indexed->second.symbol);
  if (indexed->second.side == itch::FeedSide::Buy) {
    auto level = book->second.bids.find(indexed->second.price);
    level->second.aggregate_shares -= indexed->second.order->remaining_shares;
    level->second.orders.erase(indexed->second.order);
    if (level->second.orders.empty()) {
      book->second.bids.erase(level);
    }
  } else {
    auto level = book->second.asks.find(indexed->second.price);
    level->second.aggregate_shares -= indexed->second.order->remaining_shares;
    level->second.orders.erase(indexed->second.order);
    if (level->second.orders.empty()) {
      book->second.asks.erase(level);
    }
  }
  if (book->second.bids.empty() && book->second.asks.empty()) {
    books_.erase(book);
  }
  order_index_.erase(indexed);
}

std::optional<std::string> FactualState::symbol_for_locate(const itch::StockLocate locate) const {
  const auto directory = directories_.find(locate);
  return directory == directories_.end()
             ? std::nullopt
             : std::optional<std::string>{directory->second.stock.trimmed()};
}

std::optional<char> FactualState::trading_state(const std::string &symbol) const {
  const auto state = reference_state_.find("H|" + symbol);
  if (state == reference_state_.end()) {
    return std::nullopt;
  }
  const auto *message = std::get_if<itch::StockTradingAction>(&state->second);
  return message == nullptr ? std::nullopt : std::optional<char>{message->trading_state};
}

std::vector<FactualDepthLevel> FactualState::depth(const std::string &symbol,
                                                   const itch::FeedSide side,
                                                   const std::size_t max_levels) const {
  std::vector<FactualDepthLevel> result;
  const auto book = books_.find(symbol);
  if (book == books_.end()) {
    return result;
  }
  const auto append = [max_levels, &result](const auto &levels) {
    for (const auto &[price, level] : levels) {
      if (max_levels != 0 && result.size() == max_levels) {
        break;
      }
      result.push_back(FactualDepthLevel{price, level.aggregate_shares, level.orders.size()});
    }
  };
  if (side == itch::FeedSide::Buy) {
    append(book->second.bids);
  } else {
    append(book->second.asks);
  }
  return result;
}

std::vector<FactualOrderView> FactualState::active_orders() const {
  std::vector<FactualOrderView> result;
  result.reserve(order_index_.size());
  const auto append = [&result](const auto &levels, const itch::FeedSide side) {
    for (const auto &[price, level] : levels) {
      std::size_t position = 0;
      for (const auto &order : level.orders) {
        result.push_back(FactualOrderView{order.order_reference, order.stock_locate, order.symbol,
                                          side, price, order.remaining_shares, order.attribution,
                                          order.fifo_sequence, position++});
      }
    }
  };
  for (const auto &[symbol, book] : books_) {
    (void)symbol;
    append(book.bids, itch::FeedSide::Buy);
    append(book.asks, itch::FeedSide::Sell);
  }
  return result;
}

std::optional<FactualOrderView>
FactualState::order(const itch::OrderReference order_reference) const {
  const auto indexed = order_index_.find(order_reference);
  if (indexed == order_index_.end()) {
    return std::nullopt;
  }
  std::size_t position = 0;
  const auto &book = books_.at(indexed->second.symbol);
  if (indexed->second.side == itch::FeedSide::Buy) {
    for (auto order = book.bids.at(indexed->second.price).orders.begin();
         order != indexed->second.order; ++order) {
      ++position;
    }
  } else {
    for (auto order = book.asks.at(indexed->second.price).orders.begin();
         order != indexed->second.order; ++order) {
      ++position;
    }
  }
  const auto &value = *indexed->second.order;
  return FactualOrderView{value.order_reference,
                          value.stock_locate,
                          value.symbol,
                          value.side,
                          value.display_price,
                          value.remaining_shares,
                          value.attribution,
                          value.fifo_sequence,
                          position};
}

std::size_t FactualState::active_price_level_count() const noexcept {
  std::size_t count = 0;
  for (const auto &[symbol, book] : books_) {
    (void)symbol;
    count += book.bids.size() + book.asks.size();
  }
  return count;
}

std::string FactualState::canonical_state() const {
  std::ostringstream out;
  out << "phase=" << to_string(session_phase_) << ";applied=" << applied_messages_
      << ";next_fifo=" << next_fifo_sequence_ << ";volumes=" << volumes_.displayed_add << ','
      << volumes_.displayed_cancel << ',' << volumes_.displayed_execute << ','
      << volumes_.printable_trade << ',' << volumes_.non_printable_trade << ','
      << volumes_.broken_trade << ';';
  for (const auto &[locate, directory] : directories_) {
    out << "dir=" << locate << ':' << itch::message_canonical(itch::Message{directory}) << ';';
  }
  for (const auto &[key, message] : reference_state_) {
    out << "ref=" << key << ':' << itch::message_canonical(message) << ';';
  }
  for (const auto &[symbol, book] : books_) {
    const std::string &symbol_name = symbol;
    const SymbolBook &symbol_book = book;
    const auto append = [&out, &symbol_name](const auto &levels, const char side) {
      for (const auto &[price, level] : levels) {
        out << "level=" << symbol_name << ':' << side << ':' << price << ':'
            << level.aggregate_shares;
        for (const auto &order : level.orders) {
          out << '[' << order.order_reference << ',' << order.stock_locate << ','
              << order.remaining_shares << ',' << order.fifo_sequence << ',';
          if (order.attribution.has_value()) {
            out << *order.attribution;
          }
          out << ']';
        }
        out << ';';
      }
    };
    append(symbol_book.bids, 'B');
    append(symbol_book.asks, 'S');
  }
  for (const auto &trade : trades_) {
    out << "trade=" << trade.source_type << ':' << trade.match_number << ':' << trade.stock_locate
        << ':' << trade.symbol << ':' << trade.shares << ':' << trade.execution_price << ':'
        << (trade.printable ? 1 : 0) << ':' << (trade.broken ? 1 : 0) << ';';
  }
  return out.str();
}

std::uint64_t FactualState::digest() const {
  constexpr std::uint64_t offset_basis = 1'469'598'103'934'665'603ULL;
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  std::uint64_t value = offset_basis;
  for (const char character : canonical_state()) {
    value ^= static_cast<unsigned char>(character);
    value *= prime;
  }
  return value;
}

bool FactualState::check_invariants(std::string *error_message) const {
  const auto fail = [error_message](const std::string &message) {
    if (error_message != nullptr) {
      *error_message = message;
    }
    return false;
  };
  std::unordered_set<itch::OrderReference> reachable;
  std::size_t count = 0;
  for (const auto &[symbol, book] : books_) {
    const std::string &symbol_name = symbol;
    const SymbolBook &symbol_book = book;
    const auto check_levels = [&](const auto &levels, const itch::FeedSide side) {
      for (const auto &[price, level] : levels) {
        if (level.orders.empty()) {
          return fail("empty price level for " + symbol_name);
        }
        itch::Shares aggregate = 0;
        std::uint64_t previous_sequence = 0;
        for (const auto &order : level.orders) {
          if (order.remaining_shares == 0 || order.symbol != symbol_name || order.side != side ||
              order.display_price != price || order.fifo_sequence <= previous_sequence) {
            return fail("order metadata or FIFO invariant failure for " + symbol_name);
          }
          previous_sequence = order.fifo_sequence;
          if (!can_add(aggregate, order.remaining_shares)) {
            return fail("level aggregate overflow for " + symbol_name);
          }
          aggregate += order.remaining_shares;
          const auto directory = directories_.find(order.stock_locate);
          if (directory == directories_.end() || directory->second.stock.trimmed() != symbol_name) {
            return fail("active order lacks consistent directory mapping");
          }
          if (!reachable.insert(order.order_reference).second) {
            return fail("duplicate reachable order reference");
          }
          const auto indexed = order_index_.find(order.order_reference);
          if (indexed == order_index_.end() || indexed->second.side != side ||
              indexed->second.symbol != symbol_name || indexed->second.price != price ||
              &*indexed->second.order != &order) {
            return fail("order index does not point to owning FIFO order");
          }
          ++count;
        }
        if (aggregate != level.aggregate_shares) {
          return fail("cached level aggregate mismatch");
        }
      }
      return true;
    };
    if (!check_levels(symbol_book.bids, itch::FeedSide::Buy) ||
        !check_levels(symbol_book.asks, itch::FeedSide::Sell)) {
      return false;
    }
  }
  if (count != order_index_.size()) {
    return fail("indexed order count differs from reachable order count");
  }
  for (const auto &[reference, indexed] : order_index_) {
    if (!reachable.contains(reference) || indexed.order->order_reference != reference) {
      return fail("order index contains an unreachable reference");
    }
  }
  if (trade_index_.size() != trades_.size()) {
    return fail("trade match index size differs from ledger size");
  }
  for (const auto &[match, index] : trade_index_) {
    if (index >= trades_.size() || trades_[index].match_number != match) {
      return fail("trade match index points to wrong ledger record");
    }
  }
  for (const auto &[locate, directory] : directories_) {
    const auto symbol = symbol_locates_.find(directory.stock.trimmed());
    if (symbol == symbol_locates_.end() || symbol->second != locate) {
      return fail("directory and symbol maps disagree");
    }
  }
  return true;
}

} // namespace lob::replay
