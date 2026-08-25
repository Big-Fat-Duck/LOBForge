#pragma once

#include "lob/itch/decoder.hpp"
#include "lob/itch/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob::replay {

enum class SessionPhase : std::uint8_t {
  BeforeMessages,
  MessagesStarted,
  SystemHours,
  MarketHours,
  MarketHoursEnded,
  SystemHoursEnded,
  MessagesEnded
};

[[nodiscard]] std::string_view to_string(SessionPhase phase) noexcept;

struct FactualDepthLevel {
  itch::Price4 price{};
  itch::Shares shares{};
  std::size_t order_count{};
  friend bool operator==(const FactualDepthLevel &, const FactualDepthLevel &) = default;
};

struct FactualOrderView {
  itch::OrderReference order_reference{};
  itch::StockLocate stock_locate{};
  std::string symbol;
  itch::FeedSide side{itch::FeedSide::Buy};
  itch::Price4 display_price{};
  itch::Shares remaining_shares{};
  std::optional<std::string> attribution;
  std::uint64_t fifo_sequence{};
  std::size_t queue_position{};
  friend bool operator==(const FactualOrderView &, const FactualOrderView &) = default;
};

struct TradeRecord {
  char source_type{};
  itch::MatchNumber match_number{};
  itch::StockLocate stock_locate{};
  std::string symbol;
  itch::Shares shares{};
  itch::Price4 execution_price{};
  bool printable{};
  bool broken{};
  friend bool operator==(const TradeRecord &, const TradeRecord &) = default;
};

struct VolumeStatistics {
  std::uint64_t displayed_add{};
  std::uint64_t displayed_cancel{};
  std::uint64_t displayed_execute{};
  std::uint64_t printable_trade{};
  std::uint64_t non_printable_trade{};
  std::uint64_t broken_trade{};
  friend bool operator==(const VolumeStatistics &, const VolumeStatistics &) = default;
};

struct BookMutation {
  char message_type{};
  itch::StockLocate stock_locate{};
  std::string symbol;
  itch::FeedSide side{itch::FeedSide::Buy};
  itch::OrderReference order_reference{};
  std::optional<itch::OrderReference> new_order_reference;
  itch::Shares event_quantity{};
  itch::Price4 display_price{};
  std::optional<itch::Price4> execution_price;
  std::optional<itch::MatchNumber> match_number;
  friend bool operator==(const BookMutation &, const BookMutation &) = default;
};

struct ApplyResult {
  std::optional<itch::ParseError> error;
  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

class FactualState final {
public:
  FactualState() = default;
  FactualState(const FactualState &) = delete;
  FactualState &operator=(const FactualState &) = delete;
  FactualState(FactualState &&) = default;
  FactualState &operator=(FactualState &&) = default;
  ~FactualState() = default;

  [[nodiscard]] ApplyResult apply(const itch::Message &message,
                                  std::size_t absolute_file_offset = 0,
                                  std::size_t record_index = 0);

  [[nodiscard]] SessionPhase session_phase() const noexcept { return session_phase_; }
  [[nodiscard]] std::size_t symbol_count() const noexcept { return directories_.size(); }
  [[nodiscard]] std::size_t active_order_count() const noexcept { return order_index_.size(); }
  [[nodiscard]] std::size_t active_price_level_count() const noexcept;
  [[nodiscard]] std::size_t trade_count() const noexcept { return trades_.size(); }
  [[nodiscard]] const std::vector<TradeRecord> &trades() const noexcept { return trades_; }
  [[nodiscard]] const VolumeStatistics &volumes() const noexcept { return volumes_; }
  void capture_book_mutations(const bool enabled) noexcept {
    capture_book_mutations_ = enabled;
    if (!enabled) {
      last_book_mutation_.reset();
    }
  }
  [[nodiscard]] const std::optional<BookMutation> &last_book_mutation() const noexcept {
    return last_book_mutation_;
  }
  [[nodiscard]] std::optional<std::string> symbol_for_locate(itch::StockLocate locate) const;
  [[nodiscard]] std::optional<char> trading_state(const std::string &symbol) const;
  [[nodiscard]] std::vector<FactualDepthLevel> depth(const std::string &symbol, itch::FeedSide side,
                                                     std::size_t max_levels = 0) const;
  [[nodiscard]] std::vector<FactualOrderView> active_orders() const;
  [[nodiscard]] std::optional<FactualOrderView> order(itch::OrderReference order_reference) const;
  [[nodiscard]] std::string canonical_state() const;
  [[nodiscard]] std::uint64_t digest() const;
  [[nodiscard]] bool check_invariants(std::string *error = nullptr) const;

private:
  struct RestingOrder {
    itch::OrderReference order_reference{};
    itch::StockLocate stock_locate{};
    std::string symbol;
    itch::FeedSide side{itch::FeedSide::Buy};
    itch::Price4 display_price{};
    itch::Shares remaining_shares{};
    std::optional<std::string> attribution;
    std::uint64_t fifo_sequence{};
  };

  struct PriceLevel {
    itch::Shares aggregate_shares{};
    std::list<RestingOrder> orders;
  };

  struct SymbolBook {
    std::map<itch::Price4, PriceLevel, std::greater<itch::Price4>> bids;
    std::map<itch::Price4, PriceLevel, std::less<itch::Price4>> asks;
  };

  using OrderIterator = std::list<RestingOrder>::iterator;
  struct IndexEntry {
    itch::FeedSide side{itch::FeedSide::Buy};
    std::string symbol;
    itch::Price4 price{};
    OrderIterator order;
  };

  SessionPhase session_phase_{SessionPhase::BeforeMessages};
  std::map<itch::StockLocate, itch::StockDirectory> directories_;
  std::map<std::string, itch::StockLocate> symbol_locates_;
  std::map<std::string, itch::Message> reference_state_;
  std::map<std::string, SymbolBook> books_;
  std::unordered_map<itch::OrderReference, IndexEntry> order_index_;
  std::vector<TradeRecord> trades_;
  std::unordered_map<itch::MatchNumber, std::size_t> trade_index_;
  VolumeStatistics volumes_;
  bool capture_book_mutations_{};
  std::optional<BookMutation> last_book_mutation_;
  std::uint64_t next_fifo_sequence_{1};
  std::uint64_t applied_messages_{};

  [[nodiscard]] ApplyResult apply_system_event(const itch::SystemEvent &message,
                                               std::size_t absolute_file_offset,
                                               std::size_t record_index);
  [[nodiscard]] ApplyResult apply_directory(const itch::StockDirectory &message,
                                            std::size_t absolute_file_offset,
                                            std::size_t record_index);
  [[nodiscard]] ApplyResult apply_add(const itch::CommonHeader &header,
                                      itch::OrderReference order_reference, itch::FeedSide side,
                                      std::uint32_t shares, const itch::Stock &stock,
                                      itch::Price4 price, std::optional<std::string> attribution,
                                      char source_type, std::size_t absolute_file_offset,
                                      std::size_t record_index);
  [[nodiscard]] ApplyResult
  apply_execution(const itch::CommonHeader &header, itch::OrderReference order_reference,
                  std::uint32_t shares, itch::MatchNumber match_number,
                  std::optional<itch::Price4> execution_price, bool printable, char source_type,
                  std::size_t absolute_file_offset, std::size_t record_index);
  [[nodiscard]] ApplyResult apply_cancel(const itch::OrderCancel &message,
                                         std::size_t absolute_file_offset,
                                         std::size_t record_index);
  [[nodiscard]] ApplyResult apply_delete(const itch::OrderDelete &message,
                                         std::size_t absolute_file_offset,
                                         std::size_t record_index);
  [[nodiscard]] ApplyResult apply_replace(const itch::OrderReplace &message,
                                          std::size_t absolute_file_offset,
                                          std::size_t record_index);
  [[nodiscard]] ApplyResult apply_non_cross_trade(const itch::NonCrossTrade &message,
                                                  std::size_t absolute_file_offset,
                                                  std::size_t record_index);
  [[nodiscard]] ApplyResult apply_cross_trade(const itch::CrossTrade &message,
                                              std::size_t absolute_file_offset,
                                              std::size_t record_index);
  [[nodiscard]] ApplyResult apply_broken_trade(const itch::BrokenTrade &message,
                                               std::size_t absolute_file_offset,
                                               std::size_t record_index);
  [[nodiscard]] ApplyResult apply_reference(const itch::Message &message,
                                            std::size_t absolute_file_offset,
                                            std::size_t record_index);

  [[nodiscard]] std::optional<itch::ParseError>
  validate_locate_symbol(const itch::CommonHeader &header, const itch::Stock &stock, char type,
                         std::size_t absolute_file_offset, std::size_t record_index) const;
  [[nodiscard]] itch::ParseError error(itch::ErrorCategory category, char type,
                                       std::size_t absolute_file_offset, std::size_t record_index,
                                       std::string_view diagnostic) const;
  void erase_order(std::unordered_map<itch::OrderReference, IndexEntry>::iterator indexed);
  [[nodiscard]] bool add_volume(std::uint64_t &target, std::uint64_t quantity) const noexcept;
};

} // namespace lob::replay
