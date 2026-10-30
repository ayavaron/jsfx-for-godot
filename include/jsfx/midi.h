#pragma once

#include <cstdint>

namespace jsfx {

/// One short MIDI message. `offset` is the frame within the audio block
/// (JSFX's `midirecv`/`midisend` first argument).
struct MidiMessage {
  int offset = 0;
  std::uint8_t status = 0;
  std::uint8_t data1 = 0;
  std::uint8_t data2 = 0;
};

/// Per-block limit for MIDI in each direction. Events beyond it are dropped.
inline constexpr int kMaxMidiEventsPerBlock = 256;

}  // namespace jsfx
