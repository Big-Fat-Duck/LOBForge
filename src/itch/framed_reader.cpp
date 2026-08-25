#include "lob/itch/framed_reader.hpp"

#include <cstdint>

namespace lob::itch {

FrameResult FramedReader::next() noexcept {
  if (done()) {
    return {std::nullopt, ParseError{ErrorCategory::IncompleteEnvelope, offset_, record_index_,
                                     std::nullopt, 2, 0, "no record remains"}};
  }
  const std::size_t available = file_.size() - offset_;
  if (available < 2) {
    return {std::nullopt,
            ParseError{ErrorCategory::IncompleteEnvelope, offset_, record_index_, std::nullopt, 2,
                       available, "incomplete two-byte record envelope"}};
  }
  const auto high = std::to_integer<std::uint16_t>(file_[offset_]);
  const auto low = std::to_integer<std::uint16_t>(file_[offset_ + 1]);
  const std::size_t length = static_cast<std::size_t>((high << 8U) | low);
  const std::size_t payload_offset = offset_ + 2;
  const std::size_t payload_available = file_.size() - payload_offset;
  if (payload_available < length) {
    return {std::nullopt,
            ParseError{ErrorCategory::TruncatedFrame, offset_, record_index_, std::nullopt, length,
                       payload_available, "declared payload overruns end of file"}};
  }
  Frame frame{file_.subspan(payload_offset, length), offset_, payload_offset, record_index_};
  offset_ = payload_offset + length;
  ++record_index_;
  return {frame, std::nullopt};
}

} // namespace lob::itch
