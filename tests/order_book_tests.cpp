#include "lob/order_book.hpp"
#include "reference_order_book.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

NewOrder limit(const OrderId id, const Side side, const Price price, const Quantity quantity,
               const TimeInForce tif = TimeInForce::GTC, const Timestamp timestamp = 0) {
  return NewOrder{id, side, OrderType::Limit, tif, price, quantity, timestamp};
}

NewOrder market(const OrderId id, const Side side, const Quantity quantity,
                const TimeInForce tif = TimeInForce::IOC) {
  return NewOrder{id, side, OrderType::Market, tif, std::nullopt, quantity, 0};
}

std::vector<Event> checked(OrderBook &book, const Command &command) {
  auto events = book.process(command);
  std::string error;
  if (!book.check_invariants(&error)) {
    throw TestFailure("invariant failure: " + error);
  }
  return events;
}

template <typename T> std::vector<T> select(const std::vector<Event> &events) {
  std::vector<T> result;
  for (const auto &event : events) {
    if (const auto *value = std::get_if<T>(&event)) {
      result.push_back(*value);
    }
  }
  return result;
}

RejectReason rejection(const std::vector<Event> &events) {
  CHECK(events.size() == 1);
  CHECK(std::holds_alternative<Rejected>(events.front()));
  return std::get<Rejected>(events.front()).reason;
}

void price_priority_across_several_levels() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 103, 10));
  checked(book, limit(2, Side::Sell, 101, 10));
  checked(book, limit(3, Side::Sell, 102, 10));
  const auto trades = select<Trade>(checked(book, limit(4, Side::Buy, 103, 30)));
  CHECK(trades.size() == 3);
  CHECK(trades[0].maker_id == 2 && trades[0].resting_price == 101);
  CHECK(trades[1].maker_id == 3 && trades[1].resting_price == 102);
  CHECK(trades[2].maker_id == 1 && trades[2].resting_price == 103);
}

void fifo_priority_within_one_level() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 10));
  checked(book, limit(2, Side::Sell, 100, 10));
  const auto trades = select<Trade>(checked(book, limit(3, Side::Buy, 100, 15)));
  CHECK(trades.size() == 2);
  CHECK(trades[0].maker_id == 1 && trades[0].quantity == 10);
  CHECK(trades[1].maker_id == 2 && trades[1].quantity == 5);
}

void partial_maker_fill() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 20));
  checked(book, limit(2, Side::Buy, 100, 5));
  CHECK(book.order(1)->remaining_quantity == 15);
  CHECK(!book.order(2).has_value());
}

void partial_taker_fill() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 5));
  const auto events = checked(book, limit(2, Side::Buy, 100, 20));
  CHECK(select<Trade>(events).front().quantity == 5);
  CHECK(book.order(2)->remaining_quantity == 15);
}

void one_order_sweeps_several_levels() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 5));
  checked(book, limit(2, Side::Sell, 101, 7));
  checked(book, limit(3, Side::Sell, 102, 9));
  const auto trades = select<Trade>(checked(book, market(4, Side::Buy, 20)));
  CHECK(trades.size() == 3);
  CHECK(trades[2].quantity == 8);
  CHECK(book.quantity_at(Side::Sell, 102) == 1);
}

void trade_price_equals_resting_price() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 99, 4));
  const auto trade = select<Trade>(checked(book, limit(2, Side::Buy, 105, 4))).front();
  CHECK(trade.resting_price == 99);
}

void gtc_remainder_rests() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 5));
  const auto events = checked(book, limit(2, Side::Buy, 100, 8));
  CHECK(select<Rested>(events).front().remaining_quantity == 3);
  CHECK(book.best_bid() == 100);
}

void ioc_remainder_does_not_rest() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 5));
  const auto events = checked(book, limit(2, Side::Buy, 100, 8, TimeInForce::IOC));
  CHECK(select<Expired>(events).front().expired_quantity == 3);
  CHECK(!book.order(2).has_value());
}

void fok_success() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 5));
  checked(book, limit(2, Side::Sell, 101, 5));
  const auto trades = select<Trade>(checked(book, limit(3, Side::Buy, 101, 10, TimeInForce::FOK)));
  CHECK(trades.size() == 2);
  CHECK(book.active_order_count() == 0);
}

void fok_failure_is_atomic() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 5));
  const std::string before = book.canonical_snapshot();
  const auto events = checked(book, limit(2, Side::Buy, 100, 6, TimeInForce::FOK));
  CHECK(rejection(events) == RejectReason::CannotFullyFill);
  CHECK(book.canonical_snapshot() == before);
}

void post_only_accept_and_reject() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 101, 5));
  CHECK(select<Rested>(checked(book, limit(2, Side::Buy, 100, 5, TimeInForce::PostOnly))).size() ==
        1);
  const std::string before = book.canonical_snapshot();
  CHECK(rejection(checked(book, limit(3, Side::Buy, 101, 5, TimeInForce::PostOnly))) ==
        RejectReason::WouldCross);
  CHECK(book.canonical_snapshot() == before);
}

void market_order_remainder_expires() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 2));
  const auto expired = select<Expired>(checked(book, market(2, Side::Buy, 5)));
  CHECK(expired.size() == 1 && expired[0].expired_quantity == 3);
  CHECK(expired[0].reason == ExpireReason::MarketRemainder);
}

void complete_cancellation() {
  OrderBook book;
  checked(book, limit(1, Side::Buy, 100, 10));
  const auto cancelled = select<Cancelled>(checked(book, CancelOrder{1}));
  CHECK(cancelled.size() == 1 && cancelled[0].cancelled_quantity == 10);
  CHECK(book.active_order_count() == 0 && !book.best_bid().has_value());
}

void quantity_reduction_retains_priority() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 10));
  checked(book, limit(2, Side::Sell, 100, 10));
  const auto sequence = book.order(1)->priority_sequence;
  checked(book, ReduceOrder{1, 4});
  CHECK(book.order(1)->priority_sequence == sequence);
  const auto trades = select<Trade>(checked(book, limit(3, Side::Buy, 100, 8)));
  CHECK(trades[0].maker_id == 1 && trades[1].maker_id == 2);
}

void same_price_quantity_increase_loses_priority() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 5));
  checked(book, limit(2, Side::Sell, 100, 5));
  const auto replacement = select<Replaced>(checked(book, ReplaceOrder{1, 100, 6, 0}));
  CHECK(replacement.size() == 1 && !replacement[0].priority_retained);
  const auto trades = select<Trade>(checked(book, limit(3, Side::Buy, 100, 6)));
  CHECK(trades[0].maker_id == 2);
  CHECK(trades[1].maker_id == 1);
}

void same_price_smaller_replacement_retains_priority() {
  OrderBook book;
  checked(book, limit(1, Side::Sell, 100, 10));
  checked(book, limit(2, Side::Sell, 100, 10));
  const auto before = book.order(1)->priority_sequence;
  const auto event = select<Replaced>(checked(book, ReplaceOrder{1, 100, 6, 9})).front();
  CHECK(event.priority_retained);
  CHECK(book.order(1)->priority_sequence == before);
  const auto trades = select<Trade>(checked(book, limit(3, Side::Buy, 100, 8)));
  CHECK(trades[0].maker_id == 1 && trades[0].quantity == 6);
  CHECK(trades[1].maker_id == 2 && trades[1].quantity == 2);
}

void price_replacement_loses_priority_and_crosses() {
  OrderBook book;
  checked(book, limit(1, Side::Buy, 99, 10));
  checked(book, limit(2, Side::Sell, 101, 4));
  const auto events = checked(book, ReplaceOrder{1, 101, 10, 7});
  CHECK(!select<Replaced>(events).front().priority_retained);
  const auto trade = select<Trade>(events).front();
  CHECK(trade.maker_id == 2 && trade.taker_id == 1 && trade.resting_price == 101);
  CHECK(book.order(1)->remaining_quantity == 6);
}

void failed_replacement_preserves_original() {
  OrderBook book;
  checked(book, limit(1, Side::Buy, 101, 1));
  checked(book, limit(2, Side::Buy, 100, std::numeric_limits<Quantity>::max()));
  const auto before = book.order(1);
  CHECK(rejection(checked(book, ReplaceOrder{1, 100, 2, 0})) == RejectReason::AggregateOverflow);
  CHECK(book.order(1) == before);
}

void duplicate_id_rejection() {
  OrderBook book;
  checked(book, limit(1, Side::Buy, 100, 1));
  const auto before = book.canonical_snapshot();
  CHECK(rejection(checked(book, limit(1, Side::Sell, 101, 1))) == RejectReason::DuplicateOrderId);
  CHECK(book.canonical_snapshot() == before);
}

void unknown_id_operations() {
  OrderBook book;
  CHECK(rejection(checked(book, CancelOrder{91})) == RejectReason::UnknownOrderId);
  CHECK(rejection(checked(book, ReduceOrder{91, 1})) == RejectReason::UnknownOrderId);
  CHECK(rejection(checked(book, ReplaceOrder{91, 100, 1, 0})) == RejectReason::UnknownOrderId);
}

void invalid_and_zero_quantities() {
  OrderBook book;
  CHECK(rejection(checked(book, limit(1, Side::Buy, 100, 0))) == RejectReason::ZeroQuantity);
  CHECK(rejection(checked(book, NewOrder{2, Side::Buy, OrderType::Limit, TimeInForce::GTC,
                                         std::nullopt, 1, 0})) == RejectReason::InvalidPrice);
  CHECK(rejection(checked(book, NewOrder{3, Side::Buy, OrderType::Market, TimeInForce::GTC,
                                         std::nullopt, 1, 0})) ==
        RejectReason::InvalidOrderTypeTimeInForce);
  CHECK(rejection(checked(book, limit(30, Side::Buy, 0, 1))) == RejectReason::InvalidPrice);
  CHECK(rejection(checked(book, limit(31, Side::Buy, -1, 1))) == RejectReason::InvalidPrice);
  CHECK(rejection(checked(book, NewOrder{32, Side::Buy, OrderType::Market, TimeInForce::IOC,
                                         Price{100}, 1, 0})) == RejectReason::InvalidPrice);
  CHECK(rejection(checked(book, NewOrder{33, Side::Buy, OrderType::Market, TimeInForce::PostOnly,
                                         std::nullopt, 1, 0})) ==
        RejectReason::InvalidOrderTypeTimeInForce);
  checked(book, limit(4, Side::Buy, 100, 2));
  CHECK(rejection(checked(book, ReduceOrder{4, 0})) == RejectReason::InvalidReduction);
  CHECK(rejection(checked(book, ReduceOrder{4, 2})) == RejectReason::InvalidReduction);
  CHECK(rejection(checked(book, ReduceOrder{4, 3})) == RejectReason::InvalidReduction);
  const auto original = book.order(4);
  CHECK(rejection(checked(book, ReplaceOrder{4, 100, 0, 0})) == RejectReason::ZeroQuantity);
  CHECK(rejection(checked(book, ReplaceOrder{4, -1, 1, 0})) == RejectReason::InvalidPrice);
  CHECK(book.order(4) == original);
}

void empty_book_behavior() {
  OrderBook book;
  CHECK(!book.best_bid().has_value() && !book.best_ask().has_value());
  CHECK(book.snapshot() == BookSnapshot{});
  const auto expired = select<Expired>(checked(book, market(1, Side::Buy, 5)));
  CHECK(expired.size() == 1 && expired[0].expired_quantity == 5);
}

void deterministic_event_ordering_and_sequences() {
  OrderBook book;
  std::vector<Event> all;
  for (const Command &command :
       std::vector<Command>{limit(1, Side::Sell, 100, 2), limit(2, Side::Sell, 101, 2),
                            limit(3, Side::Buy, 101, 5)}) {
    const auto events = checked(book, command);
    all.insert(all.end(), events.begin(), events.end());
  }
  for (std::size_t index = 0; index < all.size(); ++index) {
    CHECK(event_sequence(all[index]) == index + 1);
  }
  const auto trades = select<Trade>(all);
  CHECK(trades[0].maker_id == 1 && trades[1].maker_id == 2);
}

void canonical_snapshot_equality_across_repeated_runs() {
  const std::vector<Command> commands{limit(1, Side::Buy, 99, 20),
                                      limit(2, Side::Sell, 102, 15),
                                      limit(3, Side::Sell, 101, 4),
                                      limit(4, Side::Buy, 102, 9),
                                      ReduceOrder{1, 3},
                                      ReplaceOrder{2, 103, 12, 55},
                                      CancelOrder{1},
                                      market(5, Side::Sell, 2)};
  std::vector<Event> baseline_events;
  std::vector<std::string> baseline_snapshots;
  for (int run = 0; run < 10; ++run) {
    OrderBook book;
    std::vector<Event> run_events;
    std::vector<std::string> run_snapshots;
    for (const auto &command : commands) {
      auto events = checked(book, command);
      run_events.insert(run_events.end(), events.begin(), events.end());
      run_snapshots.push_back(book.canonical_snapshot());
    }
    if (run == 0) {
      baseline_events = run_events;
      baseline_snapshots = run_snapshots;
    } else {
      CHECK(run_events == baseline_events);
      CHECK(run_snapshots == baseline_snapshots);
    }
  }
}

void arithmetic_boundaries_and_overflow_rejection() {
  OrderBook book;
  checked(book, limit(1, Side::Buy, 100, std::numeric_limits<Quantity>::max()));
  const auto before = book.canonical_snapshot();
  CHECK(rejection(checked(book, limit(2, Side::Buy, 100, 1))) == RejectReason::AggregateOverflow);
  CHECK(book.canonical_snapshot() == before);
  CHECK(rejection(checked(book, ReduceOrder{1, std::numeric_limits<Quantity>::max()})) ==
        RejectReason::InvalidReduction);
}

void same_price_same_quantity_replace_is_noop() {
  OrderBook book;
  checked(book, limit(1, Side::Buy, 100, 7));
  const auto before = book.order(1);
  const auto event = select<Replaced>(checked(book, ReplaceOrder{1, 100, 7, 999})).front();
  CHECK(event.priority_retained);
  CHECK(book.order(1) == before);
}

std::string describe(const Command &command) {
  return std::visit(
      [](const auto &value) {
        std::ostringstream out;
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, NewOrder>) {
          out << "new(id=" << value.order_id << ",side=" << static_cast<int>(value.side)
              << ",type=" << static_cast<int>(value.type)
              << ",tif=" << static_cast<int>(value.time_in_force) << ",price=";
          if (value.limit_price.has_value()) {
            out << *value.limit_price;
          } else {
            out << "none";
          }
          out << ",qty=" << value.quantity << ')';
        } else if constexpr (std::is_same_v<Value, CancelOrder>) {
          out << "cancel(id=" << value.order_id << ')';
        } else if constexpr (std::is_same_v<Value, ReduceOrder>) {
          out << "reduce(id=" << value.order_id << ",by=" << value.reduce_by << ')';
        } else {
          out << "replace(id=" << value.order_id << ",price=" << value.new_price
              << ",qty=" << value.new_quantity << ')';
        }
        return out.str();
      },
      command);
}

OrderId choose_id(std::mt19937_64 &rng, const OrderBook &book, const bool prefer_active) {
  const auto active = book.active_orders();
  if (prefer_active && !active.empty() && (rng() % 100) < 80) {
    return active[static_cast<std::size_t>(rng() % active.size())].order_id;
  }
  return 1'000'000 + (rng() % 10'000);
}

Command random_command(std::mt19937_64 &rng, const OrderBook &book, OrderId &next_id,
                       const std::uint64_t command_index) {
  const auto roll = rng() % 100;
  if (roll < 45) {
    const bool duplicate = !book.active_orders().empty() && (rng() % 100) < 8;
    const OrderId id = duplicate ? choose_id(rng, book, true) : next_id++;
    const Side side = (rng() & 1U) == 0 ? Side::Buy : Side::Sell;
    Quantity quantity = 1 + (rng() % 500);
    if ((rng() % 100) < 3) {
      quantity = 0;
    }
    if ((rng() % 100) < 15) {
      const auto tif_roll = rng() % 5;
      const auto tif = tif_roll == 0   ? TimeInForce::GTC
                       : tif_roll == 1 ? TimeInForce::PostOnly
                       : tif_roll == 2 ? TimeInForce::FOK
                                       : TimeInForce::IOC;
      return NewOrder{id, side, OrderType::Market, tif, std::nullopt, quantity, command_index};
    }
    Price price = 9'950 + static_cast<Price>(rng() % 101);
    if ((rng() % 100) < 2) {
      price = 0;
    }
    const auto tif_roll = rng() % 10;
    const auto tif = tif_roll < 6    ? TimeInForce::GTC
                     : tif_roll < 8  ? TimeInForce::IOC
                     : tif_roll == 8 ? TimeInForce::FOK
                                     : TimeInForce::PostOnly;
    return limit(id, side, price, quantity, tif, command_index);
  }
  if (roll < 65) {
    return CancelOrder{choose_id(rng, book, true)};
  }
  if (roll < 80) {
    const OrderId id = choose_id(rng, book, true);
    Quantity reduce_by = rng() % 600;
    if ((rng() % 10) == 0) {
      reduce_by = 0;
    }
    return ReduceOrder{id, reduce_by};
  }
  if (roll < 95) {
    const OrderId id = choose_id(rng, book, true);
    Price price = 9'950 + static_cast<Price>(rng() % 101);
    Quantity quantity = rng() % 600;
    if ((rng() % 50) == 0) {
      price = 0;
    }
    return ReplaceOrder{id, price, quantity, command_index};
  }
  const Side side = (rng() & 1U) == 0 ? Side::Buy : Side::Sell;
  return market(next_id++, side, 1 + rng() % 1'000,
                (rng() & 1U) == 0 ? TimeInForce::IOC : TimeInForce::FOK);
}

void differential_randomized_100k_20_seeds() {
  constexpr std::array<std::uint64_t, 20> seeds{
      1,     7,      19,     42,     73,      101,     313,     997,       2'003,     4'099,
      8'191, 16'381, 32'749, 65'521, 131'071, 262'139, 524'287, 1'000'003, 4'294'967, 9'999'991};
  constexpr std::size_t commands_per_seed = 5'000;
  for (const auto seed : seeds) {
    std::mt19937_64 rng(seed);
    OrderBook production;
    lob::test::ReferenceOrderBook reference;
    OrderId next_id = 1;
    std::vector<std::string> context;
    for (std::size_t index = 0; index < commands_per_seed; ++index) {
      const Command command = random_command(rng, production, next_id, index);
      context.push_back(describe(command));
      if (context.size() > 12) {
        context.erase(context.begin());
      }
      const auto actual_events = production.process(command);
      const auto expected_events = reference.process(command);
      std::string invariant_error;
      auto actual_orders = production.active_orders();
      auto expected_orders = reference.active_orders();
      const auto by_id = [](const OrderView &left, const OrderView &right) {
        return left.order_id < right.order_id;
      };
      std::sort(actual_orders.begin(), actual_orders.end(), by_id);
      const bool differs =
          actual_events != expected_events || production.best_bid() != reference.best_bid() ||
          production.best_ask() != reference.best_ask() ||
          production.snapshot() != reference.snapshot() || actual_orders != expected_orders ||
          !production.check_invariants(&invariant_error);
      if (differs) {
        std::ostringstream message;
        message << "differential mismatch seed=" << seed << " command_index=" << index
                << " invariant='" << invariant_error << "'\nreproduction context:\n";
        for (const auto &line : context) {
          message << "  " << line << '\n';
        }
        message << "production snapshot: " << production.canonical_snapshot();
        throw TestFailure(message.str());
      }
    }
  }
}

struct TestCase {
  std::string_view name;
  void (*function)();
};

} // namespace

int main() {
  const std::vector<TestCase> tests{
      {"price_priority_across_several_levels", price_priority_across_several_levels},
      {"fifo_priority_within_one_level", fifo_priority_within_one_level},
      {"partial_maker_fill", partial_maker_fill},
      {"partial_taker_fill", partial_taker_fill},
      {"one_order_sweeps_several_levels", one_order_sweeps_several_levels},
      {"trade_price_equals_resting_price", trade_price_equals_resting_price},
      {"gtc_remainder_rests", gtc_remainder_rests},
      {"ioc_remainder_does_not_rest", ioc_remainder_does_not_rest},
      {"fok_success", fok_success},
      {"fok_failure_is_atomic", fok_failure_is_atomic},
      {"post_only_accept_and_reject", post_only_accept_and_reject},
      {"market_order_remainder_expires", market_order_remainder_expires},
      {"complete_cancellation", complete_cancellation},
      {"quantity_reduction_retains_priority", quantity_reduction_retains_priority},
      {"same_price_quantity_increase_loses_priority", same_price_quantity_increase_loses_priority},
      {"same_price_smaller_replacement_retains_priority",
       same_price_smaller_replacement_retains_priority},
      {"price_replacement_loses_priority_and_crosses",
       price_replacement_loses_priority_and_crosses},
      {"failed_replacement_preserves_original", failed_replacement_preserves_original},
      {"duplicate_id_rejection", duplicate_id_rejection},
      {"unknown_id_operations", unknown_id_operations},
      {"invalid_and_zero_quantities", invalid_and_zero_quantities},
      {"empty_book_behavior", empty_book_behavior},
      {"deterministic_event_ordering_and_sequences", deterministic_event_ordering_and_sequences},
      {"canonical_snapshot_equality_across_repeated_runs",
       canonical_snapshot_equality_across_repeated_runs},
      {"arithmetic_boundaries_and_overflow_rejection",
       arithmetic_boundaries_and_overflow_rejection},
      {"same_price_same_quantity_replace_is_noop", same_price_same_quantity_replace_is_noop},
      {"differential_randomized_100k_20_seeds", differential_randomized_100k_20_seeds},
  };

  std::size_t passed = 0;
  for (const auto &test : tests) {
    try {
      test.function();
      ++passed;
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &error) {
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
      std::cerr << passed << '/' << tests.size() << " tests passed\n";
      return 1;
    }
  }
  std::cout << passed << '/' << tests.size()
            << " tests passed; randomized_commands=100000 seeds=20\n";
  return 0;
}
