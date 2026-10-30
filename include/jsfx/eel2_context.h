#pragma once

#include <memory>
#include <string_view>

#include "jsfx/document.h"
#include "jsfx/environment.h"
#include "jsfx/midi.h"
#include "jsfx/result.h"

namespace jsfx {

/// 8 Mi EEL2 memory slots (64 MiB), allocated once on the main thread.
/// WDL rounds RAM to 65536-slot blocks, so this is exact.
inline constexpr int kEelRamSlots = 8 * 1024 * 1024;

/// Low-level wrapper around one WDL EEL2 VM scoped to one JSFX document.
///
/// Construction compiles all sections and runs `@init` followed by the initial
/// `@slider`. All parsing, compilation and RAM allocation happen on the calling
/// (main) thread. The resulting instance may then be used by the audio thread.
/// This class is not thread-safe.
class Eel2Context {
 public:
  static Result<std::unique_ptr<Eel2Context>> create(
      const JsfxDocument& document, const JsfxEnvironment& environment = {});

  ~Eel2Context();

  Eel2Context(const Eel2Context&) = delete;
  Eel2Context& operator=(const Eel2Context&) = delete;

  /// Apply and read 1-based slider values. `set_slider` does not run
  /// `@slider`; callers batch changes and call `run_slider()` once per block.
  bool set_slider(int index, double value);
  double slider(int index) const;

  /// Environment variables that scripts commonly read. They take effect on the
  /// next `@block`/`@sample`; changing them does not run `@slider`.
  bool set_tempo(double bpm);
  bool set_play_state(int state);

  /// `run_block` publishes the block size to `samplesblock`, then runs `@block`.
  void run_block(int frames);
  void run_sample();
  void run_slider();

  /// MIDI for the next block: `push_midi_input` queues an event for
  /// `midirecv`; `clear_midi` drops unread input and sent output.
  bool push_midi_input(const MidiMessage& message);
  int midi_output_count() const;
  MidiMessage midi_output(int index) const;
  void clear_midi();

  bool has_section(JsfxSectionKind kind) const;

  /// Debug/escape hatch: the EEL2 variable's storage, or nullptr. The pointer
  /// is valid for this context's lifetime. Allocates when converting `name`,
  /// so do not call it from the audio thread.
  double* variable(std::string_view name);

 private:
  struct Impl;
  explicit Eel2Context(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace jsfx
