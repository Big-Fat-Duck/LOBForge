#pragma once

#include "lob/itch/decoder.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace lob::itch {

struct Frame {
  std::span<const std::byte> payload;
  std::size_t envelope_offset{};
  std::size_t payload_offset{};
  std::size_t record_index{};
};

struct FrameResult {
  std::optional<Frame> frame;
  std::optional<ParseError> error;
  [[nodiscard]] bool ok() const noexcept { return frame.has_value(); }
};

class FramedReader final {
public:
  explicit FramedReader(std::span<const std::byte> file) noexcept : file_(file) {}

  [[nodiscard]] FrameResult next() noexcept;
  [[nodiscard]] bool done() const noexcept { return offset_ == file_.size(); }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t record_index() const noexcept { return record_index_; }

private:
  std::span<const std::byte> file_;
  std::size_t offset_{};
  std::size_t record_index_{};
};

} // namespace lob::itch
