#pragma once

#include "lob/itch/framed_reader.hpp"
#include "lob/replay/factual_book.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lob::replay {

enum class ReplayMode : std::uint8_t { Strict, Permissive };

using ApplyObserver =
    std::function<void(const itch::Message &, const itch::Frame &, const FactualState &)>;

struct ReplayStatistics {
  std::size_t input_bytes{};
  std::uint64_t records_seen{};
  std::uint64_t records_decoded{};
  std::uint64_t records_applied{};
  std::uint64_t records_skipped{};
  std::uint64_t records_failed{};
  std::map<char, std::uint64_t> message_type_counts;
  std::optional<itch::FeedTimestamp> first_timestamp;
  std::optional<itch::FeedTimestamp> last_timestamp;
  std::map<itch::ErrorCategory, std::uint64_t> warning_counts;
  std::map<itch::ErrorCategory, std::uint64_t> error_counts;
};

class Replayer final {
public:
  [[nodiscard]] bool run(std::span<const std::byte> file, ReplayMode mode,
                         std::optional<std::size_t> max_messages = std::nullopt,
                         const ApplyObserver &observer = {});

  [[nodiscard]] const FactualState &state() const noexcept { return state_; }
  [[nodiscard]] const ReplayStatistics &statistics() const noexcept { return statistics_; }
  [[nodiscard]] const std::vector<itch::ParseError> &diagnostics() const noexcept {
    return diagnostics_;
  }
  [[nodiscard]] bool terminal_failure() const noexcept { return terminal_failure_; }
  [[nodiscard]] bool invariant_failure() const noexcept { return invariant_failure_; }
  [[nodiscard]] std::string canonical_state() const;
  [[nodiscard]] std::uint64_t digest() const;

private:
  FactualState state_;
  ReplayStatistics statistics_;
  std::vector<itch::ParseError> diagnostics_;
  bool terminal_failure_{};
  bool invariant_failure_{};

  void record_diagnostic(const itch::ParseError &error, ReplayMode mode, bool terminal);
};

[[nodiscard]] std::string render_text(const Replayer &replayer,
                                      const std::optional<std::string> &symbol,
                                      std::size_t top_levels);
[[nodiscard]] std::string render_json(const Replayer &replayer,
                                      const std::optional<std::string> &symbol,
                                      std::size_t top_levels);

} // namespace lob::replay
