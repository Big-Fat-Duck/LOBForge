#pragma once

#include "lob/itch/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace lob::itch {

class ByteReader final {
public:
  explicit ByteReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }

  [[nodiscard]] bool read_u8(std::uint8_t &value) noexcept {
    if (remaining() < 1) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(bytes_[position_++]);
    return true;
  }

  [[nodiscard]] bool read_be16(std::uint16_t &value) noexcept {
    std::uint64_t wide = 0;
    if (!read_be(wide, 2)) {
      return false;
    }
    value = static_cast<std::uint16_t>(wide);
    return true;
  }

  [[nodiscard]] bool read_be32(std::uint32_t &value) noexcept {
    std::uint64_t wide = 0;
    if (!read_be(wide, 4)) {
      return false;
    }
    value = static_cast<std::uint32_t>(wide);
    return true;
  }

  [[nodiscard]] bool read_be48(std::uint64_t &value) noexcept { return read_be(value, 6); }
  [[nodiscard]] bool read_be64(std::uint64_t &value) noexcept { return read_be(value, 8); }

  template <std::size_t N> [[nodiscard]] bool read_ascii(FixedAscii<N> &value) noexcept {
    if (remaining() < N) {
      return false;
    }
    for (std::size_t index = 0; index < N; ++index) {
      value.raw[index] =
          static_cast<char>(std::to_integer<unsigned char>(bytes_[position_ + index]));
    }
    position_ += N;
    return true;
  }

  [[nodiscard]] bool read_char(char &value) noexcept {
    std::uint8_t byte = 0;
    if (!read_u8(byte)) {
      return false;
    }
    value = static_cast<char>(byte);
    return true;
  }

private:
  std::span<const std::byte> bytes_;
  std::size_t position_{};

  [[nodiscard]] bool read_be(std::uint64_t &value, std::size_t width) noexcept {
    if (remaining() < width) {
      return false;
    }
    value = 0;
    for (std::size_t index = 0; index < width; ++index) {
      value = (value << 8U) | std::to_integer<std::uint8_t>(bytes_[position_++]);
    }
    return true;
  }
};

} // namespace lob::itch
