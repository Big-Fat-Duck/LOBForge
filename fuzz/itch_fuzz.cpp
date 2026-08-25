#include "lob/itch/decoder.hpp"
#include "lob/itch/framed_reader.hpp"
#include "lob/replay/replayer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, const std::size_t size) {
  const auto bytes = std::span<const std::byte>{reinterpret_cast<const std::byte *>(data), size};

  lob::itch::FramedReader reader(bytes);
  while (!reader.done()) {
    const auto frame = reader.next();
    if (!frame.ok()) {
      break;
    }
    (void)lob::itch::decode_message(frame.frame->payload, frame.frame->payload_offset,
                                    frame.frame->record_index);
  }

  lob::replay::Replayer replayer;
  (void)replayer.run(bytes, lob::replay::ReplayMode::Permissive, 10'000);
  return 0;
}
