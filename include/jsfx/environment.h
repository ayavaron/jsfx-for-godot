#pragma once

#include <memory>

#include "jsfx/file_data.h"

namespace jsfx {

/// Execution environment supplied by the host before `@init` runs.
///
/// Godot has no global tempo or play-state concept, so Phase 1 defaults to
/// 120 BPM and JSFX's "playing" state. The bridge will expose setters later.
struct JsfxEnvironment {
  double sample_rate = 48000.0;
  double tempo = 120.0;   ///< Beats per minute.
  int play_state = 1;     ///< JSFX convention: 0 stopped, 1 playing, 2 paused.

  /// Where `file_open` finds files, and the decoded files shared by every
  /// context of one effect. Without a provider only cached files open;
  /// without a cache each context keeps its own.
  std::shared_ptr<JsfxFileProvider> file_provider;
  std::shared_ptr<JsfxFileCache> file_cache;
};

}  // namespace jsfx
