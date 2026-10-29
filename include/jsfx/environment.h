#pragma once

namespace jsfx {

/// Execution environment supplied by the host before `@init` runs.
///
/// Godot has no global tempo or play-state concept, so Phase 1 defaults to
/// 120 BPM and JSFX's "playing" state. The bridge will expose setters later.
struct JsfxEnvironment {
  double sample_rate = 48000.0;
  double tempo = 120.0;   ///< Beats per minute.
  int play_state = 1;     ///< JSFX convention: 0 stopped, 1 playing, 2 paused.
};

}  // namespace jsfx
