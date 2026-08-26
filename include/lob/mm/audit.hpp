#pragma once

#include "lob/mm/protocol.hpp"
#include "lob/mm/types.hpp"

#include <string>

namespace lob::mm {

[[nodiscard]] std::string render_json(const ShadowOrderEvent &event);
[[nodiscard]] std::string render_json(const ShadowFill &fill);
[[nodiscard]] std::string render_json(const InventoryEvent &event);
[[nodiscard]] std::string render_json(const SimulationSummary &summary);
[[nodiscard]] std::string render_protocol_json(const FrozenProtocol &protocol);

} // namespace lob::mm
