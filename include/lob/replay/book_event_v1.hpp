#pragma once

#include "lob/replay/replayer.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace lob::replay {

struct BookEventV1Options {
  std::string session_date;
  std::size_t depth{10};
  std::set<std::string> symbols;
};

[[nodiscard]] bool valid_session_date(const std::string &value) noexcept;
[[nodiscard]] bool mutation_selected(const FactualState &state, const BookEventV1Options &options);
[[nodiscard]] std::string render_book_event_v1_header(const BookEventV1Options &options,
                                                      std::size_t source_size);
[[nodiscard]] std::string render_book_event_v1(const BookEventV1Options &options,
                                               std::uint64_t sequence, const itch::Message &message,
                                               const itch::Frame &frame, const FactualState &state);
[[nodiscard]] std::string render_book_event_v1_summary(const Replayer &replayer,
                                                       std::uint64_t output_records);

} // namespace lob::replay
