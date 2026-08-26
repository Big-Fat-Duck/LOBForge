#pragma once

#include "lob/mm/types.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace lob::mm {

struct FrozenProtocol {
  SimulationConfig simulation;
  std::map<std::string, std::string> parsed_values;
  std::string canonical;
  std::string sha256;
};

[[nodiscard]] FrozenProtocol load_protocol(const std::filesystem::path &path);
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

} // namespace lob::mm
