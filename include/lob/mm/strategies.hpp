#pragma once

#include "lob/mm/types.hpp"

#include <cstdint>

namespace lob::mm {

class MarketMakerStrategy final {
public:
  explicit MarketMakerStrategy(StrategyKind kind) : kind_(kind) {}

  [[nodiscard]] QuotePair quote(const MarketSnapshot &market, std::int64_t inventory,
                                const SimulationConfig &config) const;
  [[nodiscard]] StrategyKind kind() const noexcept { return kind_; }

private:
  StrategyKind kind_;
};

} // namespace lob::mm
