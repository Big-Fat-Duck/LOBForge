#include "lob/itch/messages.hpp"

#include <sstream>
#include <type_traits>

namespace lob::itch {
namespace {

template <std::size_t N> std::string ascii_hex(const FixedAscii<N> &value) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(N * 2);
  for (const char character : value.raw) {
    const auto byte = static_cast<unsigned char>(character);
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0FU]);
  }
  return result;
}

template <typename T> constexpr char type_for() {
  if constexpr (std::is_same_v<T, SystemEvent>) {
    return 'S';
  } else if constexpr (std::is_same_v<T, StockDirectory>) {
    return 'R';
  } else if constexpr (std::is_same_v<T, StockTradingAction>) {
    return 'H';
  } else if constexpr (std::is_same_v<T, RegShoRestriction>) {
    return 'Y';
  } else if constexpr (std::is_same_v<T, MarketParticipantPosition>) {
    return 'L';
  } else if constexpr (std::is_same_v<T, MwcbDeclineLevels>) {
    return 'V';
  } else if constexpr (std::is_same_v<T, MwcbStatus>) {
    return 'W';
  } else if constexpr (std::is_same_v<T, IpoQuotingPeriodUpdate>) {
    return 'K';
  } else if constexpr (std::is_same_v<T, LuldAuctionCollar>) {
    return 'J';
  } else if constexpr (std::is_same_v<T, OperationalHalt>) {
    return 'h';
  } else if constexpr (std::is_same_v<T, AddOrder>) {
    return 'A';
  } else if constexpr (std::is_same_v<T, AddOrderMpid>) {
    return 'F';
  } else if constexpr (std::is_same_v<T, OrderExecuted>) {
    return 'E';
  } else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) {
    return 'C';
  } else if constexpr (std::is_same_v<T, OrderCancel>) {
    return 'X';
  } else if constexpr (std::is_same_v<T, OrderDelete>) {
    return 'D';
  } else if constexpr (std::is_same_v<T, OrderReplace>) {
    return 'U';
  } else if constexpr (std::is_same_v<T, NonCrossTrade>) {
    return 'P';
  } else if constexpr (std::is_same_v<T, CrossTrade>) {
    return 'Q';
  } else if constexpr (std::is_same_v<T, BrokenTrade>) {
    return 'B';
  } else if constexpr (std::is_same_v<T, Noii>) {
    return 'I';
  } else if constexpr (std::is_same_v<T, RetailPriceImprovement>) {
    return 'N';
  } else {
    static_assert(std::is_same_v<T, DlcrPriceDiscovery>);
    return 'O';
  }
}

} // namespace

char message_type(const Message &message) noexcept {
  return std::visit([](const auto &value) { return type_for<std::decay_t<decltype(value)>>(); },
                    message);
}

const CommonHeader &common_header(const Message &message) noexcept {
  return std::visit([](const auto &value) -> const CommonHeader & { return value.header; },
                    message);
}

std::string_view message_name(const char type) noexcept {
  switch (type) {
  case 'S':
    return "system_event";
  case 'R':
    return "stock_directory";
  case 'H':
    return "stock_trading_action";
  case 'Y':
    return "reg_sho_restriction";
  case 'L':
    return "market_participant_position";
  case 'V':
    return "mwcb_decline_levels";
  case 'W':
    return "mwcb_status";
  case 'K':
    return "ipo_quoting_period_update";
  case 'J':
    return "luld_auction_collar";
  case 'h':
    return "operational_halt";
  case 'A':
    return "add_order";
  case 'F':
    return "add_order_mpid";
  case 'E':
    return "order_executed";
  case 'C':
    return "order_executed_with_price";
  case 'X':
    return "order_cancel";
  case 'D':
    return "order_delete";
  case 'U':
    return "order_replace";
  case 'P':
    return "non_cross_trade";
  case 'Q':
    return "cross_trade";
  case 'B':
    return "broken_trade";
  case 'I':
    return "noii";
  case 'N':
    return "retail_price_improvement";
  case 'O':
    return "dlcr_price_discovery";
  default:
    return "unknown";
  }
}

std::optional<std::size_t> expected_payload_length(const char type) noexcept {
  switch (type) {
  case 'S':
  case 'W':
    return 12;
  case 'R':
    return 39;
  case 'H':
    return 25;
  case 'Y':
  case 'N':
    return 20;
  case 'L':
    return 26;
  case 'V':
  case 'J':
  case 'U':
    return 35;
  case 'K':
    return 28;
  case 'h':
    return 21;
  case 'A':
  case 'C':
    return 36;
  case 'F':
  case 'Q':
    return 40;
  case 'E':
    return 31;
  case 'X':
    return 23;
  case 'D':
  case 'B':
    return 19;
  case 'P':
    return 44;
  case 'I':
    return 50;
  case 'O':
    return 48;
  default:
    return std::nullopt;
  }
}

std::string message_canonical(const Message &message) {
  std::ostringstream out;
  out << message_type(message) << '|' << common_header(message).stock_locate << '|'
      << common_header(message).tracking_number << '|' << common_header(message).timestamp;
  std::visit(
      [&out](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SystemEvent>) {
          out << '|' << value.event_code;
        } else if constexpr (std::is_same_v<T, StockDirectory>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.market_category << '|'
              << value.financial_status << '|' << value.round_lot_size << '|'
              << value.round_lots_only << '|' << value.issue_classification << '|'
              << ascii_hex(value.issue_sub_type) << '|' << value.authenticity << '|'
              << value.short_sale_threshold_indicator << '|' << value.ipo_flag << '|'
              << value.luld_reference_price_tier << '|' << value.etp_flag << '|'
              << value.etp_leverage_factor << '|' << value.inverse_indicator;
        } else if constexpr (std::is_same_v<T, StockTradingAction>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.trading_state << '|'
              << value.reserved << '|' << ascii_hex(value.reason);
        } else if constexpr (std::is_same_v<T, RegShoRestriction>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.action;
        } else if constexpr (std::is_same_v<T, MarketParticipantPosition>) {
          out << '|' << ascii_hex(value.mpid) << '|' << ascii_hex(value.stock) << '|'
              << value.primary_market_maker << '|' << value.market_maker_mode << '|'
              << value.market_participant_state;
        } else if constexpr (std::is_same_v<T, MwcbDeclineLevels>) {
          out << '|' << value.level_1 << '|' << value.level_2 << '|' << value.level_3;
        } else if constexpr (std::is_same_v<T, MwcbStatus>) {
          out << '|' << value.breached_level;
        } else if constexpr (std::is_same_v<T, IpoQuotingPeriodUpdate>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.ipo_quotation_release_time << '|'
              << value.ipo_quotation_release_qualifier << '|' << value.ipo_price;
        } else if constexpr (std::is_same_v<T, LuldAuctionCollar>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.auction_collar_reference_price << '|'
              << value.upper_auction_collar_price << '|' << value.lower_auction_collar_price << '|'
              << value.auction_collar_extension;
        } else if constexpr (std::is_same_v<T, OperationalHalt>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.market_code << '|'
              << value.operational_halt_action;
        } else if constexpr (std::is_same_v<T, AddOrder>) {
          out << '|' << value.order_reference << '|' << static_cast<char>(value.side) << '|'
              << value.shares << '|' << ascii_hex(value.stock) << '|' << value.price;
        } else if constexpr (std::is_same_v<T, AddOrderMpid>) {
          out << '|' << value.order_reference << '|' << static_cast<char>(value.side) << '|'
              << value.shares << '|' << ascii_hex(value.stock) << '|' << value.price << '|'
              << ascii_hex(value.attribution);
        } else if constexpr (std::is_same_v<T, OrderExecuted>) {
          out << '|' << value.order_reference << '|' << value.executed_shares << '|'
              << value.match_number;
        } else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) {
          out << '|' << value.order_reference << '|' << value.executed_shares << '|'
              << value.match_number << '|' << value.printable << '|' << value.execution_price;
        } else if constexpr (std::is_same_v<T, OrderCancel>) {
          out << '|' << value.order_reference << '|' << value.cancelled_shares;
        } else if constexpr (std::is_same_v<T, OrderDelete>) {
          out << '|' << value.order_reference;
        } else if constexpr (std::is_same_v<T, OrderReplace>) {
          out << '|' << value.original_order_reference << '|' << value.new_order_reference << '|'
              << value.shares << '|' << value.price;
        } else if constexpr (std::is_same_v<T, NonCrossTrade>) {
          out << '|' << value.order_reference << '|' << static_cast<char>(value.side) << '|'
              << value.shares << '|' << ascii_hex(value.stock) << '|' << value.price << '|'
              << value.match_number;
        } else if constexpr (std::is_same_v<T, CrossTrade>) {
          out << '|' << value.shares << '|' << ascii_hex(value.stock) << '|' << value.cross_price
              << '|' << value.match_number << '|' << value.cross_type;
        } else if constexpr (std::is_same_v<T, BrokenTrade>) {
          out << '|' << value.match_number;
        } else if constexpr (std::is_same_v<T, Noii>) {
          out << '|' << value.paired_shares << '|' << value.imbalance_shares << '|'
              << value.imbalance_direction << '|' << ascii_hex(value.stock) << '|'
              << value.far_price << '|' << value.near_price << '|' << value.current_reference_price
              << '|' << value.cross_type << '|' << value.price_variation_indicator;
        } else if constexpr (std::is_same_v<T, RetailPriceImprovement>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.interest_flag;
        } else if constexpr (std::is_same_v<T, DlcrPriceDiscovery>) {
          out << '|' << ascii_hex(value.stock) << '|' << value.open_eligibility_status << '|'
              << value.minimum_allowable_price << '|' << value.maximum_allowable_price << '|'
              << value.near_execution_price << '|' << value.near_execution_time << '|'
              << value.lower_price_range_collar << '|' << value.upper_price_range_collar;
        }
      },
      message);
  return out.str();
}

} // namespace lob::itch
