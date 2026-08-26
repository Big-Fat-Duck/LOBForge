#include "lob/mm/accounting.hpp"
#include "lob/mm/queue_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace {

std::uint64_t read_u64(std::span<const std::uint8_t> data, std::size_t &position) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64 && position < data.size(); shift += 8) {
    value |= static_cast<std::uint64_t>(data[position++]) << shift;
  }
  return value;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *raw, const std::size_t size) {
  const std::span<const std::uint8_t> data{raw, size};
  std::size_t position = 0;
  const auto model = static_cast<lob::mm::QueueModel>(size == 0 ? 0 : data[position++] % 3);
  const auto side =
      size <= 1 || data[position++] % 2 == 0 ? lob::mm::ShadowSide::Buy : lob::mm::ShadowSide::Sell;
  const std::uint32_t limit = 1U + static_cast<std::uint32_t>(read_u64(data, position) % 1'000'000);
  lob::mm::QueueTracker tracker(model, side, limit);
  lob::mm::AccountingLedger ledger;
  const lob::mm::FeeConfig fees{1, 1, 1};
  while (position < data.size()) {
    const auto opcode = data[position++] % 10;
    if (opcode < 8) {
      static constexpr char types[]{'A', 'E', 'C', 'X', 'D', 'U', 'P', 'Q'};
      const auto event_side = position < data.size() && data[position++] % 2 == 0
                                  ? lob::itch::FeedSide::Buy
                                  : lob::itch::FeedSide::Sell;
      const auto reference = read_u64(data, position);
      const auto quantity = 1U + read_u64(data, position) % 10'000U;
      const auto price_delta = static_cast<std::int32_t>(read_u64(data, position) % 3U) - 1;
      const auto signed_price = static_cast<std::int64_t>(limit) + price_delta;
      const auto price = static_cast<std::uint32_t>(std::max<std::int64_t>(1, signed_price));
      lob::replay::BookMutation mutation{
          types[opcode],
          1,
          "FUZZ",
          event_side,
          reference,
          types[opcode] == 'U' ? std::optional<lob::itch::OrderReference>{reference + 1}
                               : std::nullopt,
          quantity,
          price,
          types[opcode] == 'C' ? std::optional<lob::itch::Price4>{price + 1} : std::nullopt,
          (types[opcode] == 'E' || types[opcode] == 'C')
              ? std::optional<lob::itch::MatchNumber>{reference}
              : std::nullopt};
      (void)tracker.on_mutation(mutation);
    } else {
      const auto accounting_side =
          opcode == 8 ? lob::mm::ShadowSide::Buy : lob::mm::ShadowSide::Sell;
      const auto quantity = 1U + read_u64(data, position) % 1'000U;
      (void)ledger.apply_fill(accounting_side, quantity, limit, fees);
    }
    if (!tracker.check_invariants() || !ledger.check_invariants()) {
      __builtin_trap();
    }
  }
  return 0;
}
