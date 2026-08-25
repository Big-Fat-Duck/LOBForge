#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace lob::itch {

using StockLocate = std::uint16_t;
using TrackingNumber = std::uint16_t;
using FeedTimestamp = std::uint64_t;
using OrderReference = std::uint64_t;
using MatchNumber = std::uint64_t;
using Shares = std::uint64_t;
using Price4 = std::uint32_t;

template <std::size_t N> struct FixedAscii {
  std::array<char, N> raw{};

  [[nodiscard]] std::string trimmed() const {
    std::size_t length = N;
    while (length != 0 && raw[length - 1] == ' ') {
      --length;
    }
    return std::string(raw.data(), length);
  }

  friend bool operator==(const FixedAscii &, const FixedAscii &) = default;
};

using Stock = FixedAscii<8>;
using Mpid = FixedAscii<4>;

enum class FeedSide : char { Buy = 'B', Sell = 'S' };

struct CommonHeader {
  StockLocate stock_locate{};
  TrackingNumber tracking_number{};
  FeedTimestamp timestamp{};
  friend bool operator==(const CommonHeader &, const CommonHeader &) = default;
};

struct SystemEvent {
  CommonHeader header;
  char event_code{};
  friend bool operator==(const SystemEvent &, const SystemEvent &) = default;
};

struct StockDirectory {
  CommonHeader header;
  Stock stock;
  char market_category{};
  char financial_status{};
  std::uint32_t round_lot_size{};
  char round_lots_only{};
  char issue_classification{};
  FixedAscii<2> issue_sub_type;
  char authenticity{};
  char short_sale_threshold_indicator{};
  char ipo_flag{};
  char luld_reference_price_tier{};
  char etp_flag{};
  std::uint32_t etp_leverage_factor{};
  char inverse_indicator{};
  friend bool operator==(const StockDirectory &, const StockDirectory &) = default;
};

struct StockTradingAction {
  CommonHeader header;
  Stock stock;
  char trading_state{};
  char reserved{};
  Mpid reason;
  friend bool operator==(const StockTradingAction &, const StockTradingAction &) = default;
};

struct RegShoRestriction {
  CommonHeader header;
  Stock stock;
  char action{};
  friend bool operator==(const RegShoRestriction &, const RegShoRestriction &) = default;
};

struct MarketParticipantPosition {
  CommonHeader header;
  Mpid mpid;
  Stock stock;
  char primary_market_maker{};
  char market_maker_mode{};
  char market_participant_state{};
  friend bool operator==(const MarketParticipantPosition &,
                         const MarketParticipantPosition &) = default;
};

struct MwcbDeclineLevels {
  CommonHeader header;
  std::uint64_t level_1{};
  std::uint64_t level_2{};
  std::uint64_t level_3{};
  friend bool operator==(const MwcbDeclineLevels &, const MwcbDeclineLevels &) = default;
};

struct MwcbStatus {
  CommonHeader header;
  char breached_level{};
  friend bool operator==(const MwcbStatus &, const MwcbStatus &) = default;
};

struct IpoQuotingPeriodUpdate {
  CommonHeader header;
  Stock stock;
  std::uint32_t ipo_quotation_release_time{};
  char ipo_quotation_release_qualifier{};
  Price4 ipo_price{};
  friend bool operator==(const IpoQuotingPeriodUpdate &, const IpoQuotingPeriodUpdate &) = default;
};

struct LuldAuctionCollar {
  CommonHeader header;
  Stock stock;
  Price4 auction_collar_reference_price{};
  Price4 upper_auction_collar_price{};
  Price4 lower_auction_collar_price{};
  std::uint32_t auction_collar_extension{};
  friend bool operator==(const LuldAuctionCollar &, const LuldAuctionCollar &) = default;
};

struct OperationalHalt {
  CommonHeader header;
  Stock stock;
  char market_code{};
  char operational_halt_action{};
  friend bool operator==(const OperationalHalt &, const OperationalHalt &) = default;
};

struct AddOrder {
  CommonHeader header;
  OrderReference order_reference{};
  FeedSide side{FeedSide::Buy};
  std::uint32_t shares{};
  Stock stock;
  Price4 price{};
  friend bool operator==(const AddOrder &, const AddOrder &) = default;
};

struct AddOrderMpid {
  CommonHeader header;
  OrderReference order_reference{};
  FeedSide side{FeedSide::Buy};
  std::uint32_t shares{};
  Stock stock;
  Price4 price{};
  Mpid attribution;
  friend bool operator==(const AddOrderMpid &, const AddOrderMpid &) = default;
};

struct OrderExecuted {
  CommonHeader header;
  OrderReference order_reference{};
  std::uint32_t executed_shares{};
  MatchNumber match_number{};
  friend bool operator==(const OrderExecuted &, const OrderExecuted &) = default;
};

struct OrderExecutedWithPrice {
  CommonHeader header;
  OrderReference order_reference{};
  std::uint32_t executed_shares{};
  MatchNumber match_number{};
  char printable{};
  Price4 execution_price{};
  friend bool operator==(const OrderExecutedWithPrice &, const OrderExecutedWithPrice &) = default;
};

struct OrderCancel {
  CommonHeader header;
  OrderReference order_reference{};
  std::uint32_t cancelled_shares{};
  friend bool operator==(const OrderCancel &, const OrderCancel &) = default;
};

struct OrderDelete {
  CommonHeader header;
  OrderReference order_reference{};
  friend bool operator==(const OrderDelete &, const OrderDelete &) = default;
};

struct OrderReplace {
  CommonHeader header;
  OrderReference original_order_reference{};
  OrderReference new_order_reference{};
  std::uint32_t shares{};
  Price4 price{};
  friend bool operator==(const OrderReplace &, const OrderReplace &) = default;
};

struct NonCrossTrade {
  CommonHeader header;
  OrderReference order_reference{};
  FeedSide side{FeedSide::Buy};
  std::uint32_t shares{};
  Stock stock;
  Price4 price{};
  MatchNumber match_number{};
  friend bool operator==(const NonCrossTrade &, const NonCrossTrade &) = default;
};

struct CrossTrade {
  CommonHeader header;
  std::uint64_t shares{};
  Stock stock;
  Price4 cross_price{};
  MatchNumber match_number{};
  char cross_type{};
  friend bool operator==(const CrossTrade &, const CrossTrade &) = default;
};

struct BrokenTrade {
  CommonHeader header;
  MatchNumber match_number{};
  friend bool operator==(const BrokenTrade &, const BrokenTrade &) = default;
};

struct Noii {
  CommonHeader header;
  std::uint64_t paired_shares{};
  std::uint64_t imbalance_shares{};
  char imbalance_direction{};
  Stock stock;
  Price4 far_price{};
  Price4 near_price{};
  Price4 current_reference_price{};
  char cross_type{};
  char price_variation_indicator{};
  friend bool operator==(const Noii &, const Noii &) = default;
};

struct RetailPriceImprovement {
  CommonHeader header;
  Stock stock;
  char interest_flag{};
  friend bool operator==(const RetailPriceImprovement &, const RetailPriceImprovement &) = default;
};

struct DlcrPriceDiscovery {
  CommonHeader header;
  Stock stock;
  char open_eligibility_status{};
  Price4 minimum_allowable_price{};
  Price4 maximum_allowable_price{};
  Price4 near_execution_price{};
  std::uint64_t near_execution_time{};
  Price4 lower_price_range_collar{};
  Price4 upper_price_range_collar{};
  friend bool operator==(const DlcrPriceDiscovery &, const DlcrPriceDiscovery &) = default;
};

using Message =
    std::variant<SystemEvent, StockDirectory, StockTradingAction, RegShoRestriction,
                 MarketParticipantPosition, MwcbDeclineLevels, MwcbStatus, IpoQuotingPeriodUpdate,
                 LuldAuctionCollar, OperationalHalt, AddOrder, AddOrderMpid, OrderExecuted,
                 OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace, NonCrossTrade,
                 CrossTrade, BrokenTrade, Noii, RetailPriceImprovement, DlcrPriceDiscovery>;

[[nodiscard]] char message_type(const Message &message) noexcept;
[[nodiscard]] const CommonHeader &common_header(const Message &message) noexcept;
[[nodiscard]] std::string_view message_name(char type) noexcept;
[[nodiscard]] std::optional<std::size_t> expected_payload_length(char type) noexcept;
[[nodiscard]] std::string message_canonical(const Message &message);

} // namespace lob::itch
