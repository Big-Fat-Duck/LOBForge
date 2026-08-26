#include "lob/mm/audit.hpp"

#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace lob::mm {
namespace {

std::string escape_json(const std::string_view value) {
  std::ostringstream output;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (byte) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (byte < 0x20U) {
        output << "\\u00" << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<unsigned>(byte) << std::dec;
      } else {
        output << static_cast<char>(byte);
      }
    }
  }
  return output.str();
}

template <typename Value>
void optional_integer(std::ostringstream &output, const std::optional<Value> value) {
  if (value.has_value()) {
    output << *value;
  } else {
    output << "null";
  }
}

std::optional<long double> ratio(const std::uint64_t numerator, const std::uint64_t denominator) {
  if (denominator == 0) {
    return std::nullopt;
  }
  return static_cast<long double>(numerator) / static_cast<long double>(denominator);
}

void optional_decimal(std::ostringstream &output, const std::optional<long double> value) {
  if (value.has_value()) {
    output << std::fixed << std::setprecision(9) << *value << std::defaultfloat;
  } else {
    output << "null";
  }
}

std::ostringstream json_stream() {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  return output;
}

} // namespace

std::string render_json(const ShadowOrderEvent &event) {
  auto output = json_stream();
  output << "{\"schema\":\"" << event.schema << "\",\"version\":" << event.version
         << ",\"timestamp_ns\":" << event.timestamp_ns
         << ",\"local_sequence\":" << event.local_sequence << ",\"order_id\":" << event.order_id
         << ",\"symbol\":\"" << escape_json(event.symbol) << "\",\"side\":\""
         << to_string(event.side) << "\",\"price4\":" << event.price4
         << ",\"quantity\":" << event.quantity
         << ",\"remaining_quantity\":" << event.remaining_quantity << ",\"from_state\":\""
         << to_string(event.from_state) << "\",\"to_state\":\"" << to_string(event.to_state)
         << "\",\"reason\":\"" << to_string(event.reason)
         << "\",\"queue_ahead_quantity\":" << event.queue_ahead_quantity << '}';
  return output.str();
}

std::string render_json(const ShadowFill &fill) {
  auto output = json_stream();
  output << "{\"schema\":\"" << fill.schema << "\",\"version\":" << fill.version
         << ",\"timestamp_ns\":" << fill.timestamp_ns
         << ",\"factual_sequence\":" << fill.factual_sequence
         << ",\"local_sequence\":" << fill.local_sequence << ",\"order_id\":" << fill.order_id
         << ",\"symbol\":\"" << escape_json(fill.symbol) << "\",\"side\":\"" << to_string(fill.side)
         << "\",\"quantity\":" << fill.quantity
         << ",\"accounting_price4\":" << fill.accounting_price4
         << ",\"factual_display_price4\":" << fill.factual_display_price4
         << ",\"factual_execution_price4\":";
  optional_integer(output, fill.factual_execution_price4);
  output << ",\"anchor_mid2\":";
  optional_integer(output, fill.anchor_mid2);
  output << ",\"match_number\":";
  optional_integer(output, fill.match_number);
  output << ",\"reason\":\"" << to_string(fill.reason) << "\",\"fee_nanos\":" << fill.fee_nanos
         << ",\"rebate_nanos\":" << fill.rebate_nanos << '}';
  return output.str();
}

std::string render_json(const InventoryEvent &event) {
  auto output = json_stream();
  output << "{\"schema\":\"" << event.schema << "\",\"version\":" << event.version
         << ",\"timestamp_ns\":" << event.timestamp_ns
         << ",\"local_sequence\":" << event.local_sequence << ",\"order_id\":" << event.order_id
         << ",\"inventory\":" << event.inventory
         << ",\"trade_cash_nanos\":" << event.trade_cash_nanos
         << ",\"fees_nanos\":" << event.fees_nanos << ",\"rebates_nanos\":" << event.rebates_nanos
         << ",\"realized_gross_pnl_nanos\":" << event.realized_gross_pnl_nanos
         << ",\"gross_equity_nanos\":";
  optional_integer(output, event.gross_equity_nanos);
  output << ",\"net_equity_nanos\":";
  optional_integer(output, event.net_equity_nanos);
  output << ",\"conservative_liquidation_equity_nanos\":";
  optional_integer(output, event.conservative_liquidation_equity_nanos);
  output << '}';
  return output.str();
}

std::string render_json(const SimulationSummary &summary) {
  auto output = json_stream();
  output << "{\"schema\":\"" << summary.schema << "\",\"version\":" << summary.version
         << ",\"protocol_sha256\":\"" << summary.protocol_sha256
         << "\",\"factual_events\":" << summary.factual_events
         << ",\"order_event_rows\":" << summary.order_event_rows
         << ",\"inventory_event_rows\":" << summary.inventory_event_rows
         << ",\"submitted_orders\":" << summary.submitted_orders
         << ",\"submitted_quantity\":" << summary.submitted_quantity
         << ",\"acknowledged_orders\":" << summary.acknowledged_orders
         << ",\"rejected_orders\":" << summary.rejected_orders
         << ",\"cancelled_orders\":" << summary.cancelled_orders
         << ",\"replaced_orders\":" << summary.replaced_orders
         << ",\"filled_orders\":" << summary.filled_orders
         << ",\"fill_events\":" << summary.fill_events
         << ",\"filled_quantity\":" << summary.filled_quantity
         << ",\"turnover_quantity\":" << summary.turnover_quantity << ",\"order_fill_rate\":";
  optional_decimal(output, ratio(summary.filled_orders, summary.acknowledged_orders));
  output << ",\"quantity_fill_rate\":";
  optional_decimal(output, ratio(summary.filled_quantity, summary.submitted_quantity));
  output << ",\"cancel_replace_rate\":";
  optional_decimal(output, ratio(summary.cancelled_orders + summary.replaced_orders,
                                 summary.acknowledged_orders));
  output << ",\"risk_suppressions\":" << summary.risk_suppressions
         << ",\"stop_switch_triggers\":" << summary.stop_switch_triggers
         << ",\"final_inventory\":" << summary.final_inventory
         << ",\"maximum_absolute_inventory\":" << summary.maximum_absolute_inventory
         << ",\"inventory_limit_utilization\":";
  optional_decimal(output, summary.inventory_limit == 0
                               ? std::nullopt
                               : std::optional<long double>{
                                     static_cast<long double>(summary.maximum_absolute_inventory) /
                                     static_cast<long double>(summary.inventory_limit)});
  output << ",\"trade_cash_nanos\":" << summary.trade_cash_nanos
         << ",\"fees_nanos\":" << summary.fees_nanos
         << ",\"rebates_nanos\":" << summary.rebates_nanos << ",\"gross_pnl_nanos\":";
  optional_integer(output, summary.gross_pnl_nanos);
  output << ",\"net_pnl_nanos\":";
  optional_integer(output, summary.net_pnl_nanos);
  output << ",\"conservative_liquidation_pnl_nanos\":";
  optional_integer(output, summary.conservative_liquidation_pnl_nanos);
  output << ",\"realized_gross_pnl_nanos\":" << summary.realized_gross_pnl_nanos
         << ",\"unrealized_gross_pnl_nanos\":";
  optional_integer(output, summary.unrealized_gross_pnl_nanos);
  output << ",\"spread_capture_nanos\":" << summary.spread_capture_nanos
         << ",\"maximum_drawdown_nanos\":" << summary.maximum_drawdown_nanos
         << ",\"quoted_time_ns\":" << summary.quoted_time_ns
         << ",\"eligible_market_time_ns\":" << summary.eligible_market_time_ns
         << ",\"quote_online_rate\":";
  optional_decimal(output, ratio(summary.quoted_time_ns, summary.eligible_market_time_ns));
  output << ",\"time_weighted_absolute_inventory\":" << std::fixed << std::setprecision(9)
         << summary.time_weighted_absolute_inventory
         << ",\"rms_inventory\":" << summary.rms_inventory << std::defaultfloat
         << ",\"markouts\":[";
  for (std::size_t index = 0; index < summary.markouts.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    const auto &markout = summary.markouts[index];
    output << "{\"horizon_ns\":" << markout.horizon_ns
           << ",\"eligible_fills\":" << markout.eligible_fills
           << ",\"eligible_quantity\":" << markout.eligible_quantity
           << ",\"missing_fills\":" << markout.missing_fills
           << ",\"directional_value_nanos\":" << markout.directional_value_nanos
           << ",\"adverse_selection_cost_nanos\":" << markout.adverse_selection_cost_nanos
           << ",\"realized_spread_nanos_per_share\":";
    optional_decimal(output,
                     markout.eligible_quantity == 0
                         ? std::nullopt
                         : std::optional<long double>{
                               2.0L * static_cast<long double>(markout.directional_value_nanos) /
                               static_cast<long double>(markout.eligible_quantity)});
    output << '}';
  }
  output << "],\"semantic_digest\":\"" << summary.semantic_digest << "\"}";
  return output.str();
}

std::string render_protocol_json(const FrozenProtocol &protocol) {
  auto output = json_stream();
  output << "{\"schema\":\"lobforge.mm_protocol\",\"version\":1,\"sha256\":\"" << protocol.sha256
         << "\",\"parsed\":{";
  bool first = true;
  for (const auto &[key, value] : protocol.parsed_values) {
    if (!first) {
      output << ',';
    }
    first = false;
    output << '"' << escape_json(key) << "\":\"" << escape_json(value) << '"';
  }
  output << "}}";
  return output.str();
}

} // namespace lob::mm
