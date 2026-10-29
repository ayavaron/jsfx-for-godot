#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "jsfx/audio_frame.h"
#include "jsfx/document.h"
#include "jsfx/environment.h"
#include "jsfx/result.h"

namespace jsfx {

/// End-to-end JSFX engine: parsed document + EEL2 context + block/sample loop.
///
/// Threading contract:
///   - `create` (parse, compile, `@init`, EEL2 RAM allocation) runs on the main
///     thread.
///   - `process` is allocation-free and intended for the audio thread.
///   - `set_slider` records a pending value that is applied at the start of the
///     next `process` call. Phase 1 is single-threaded; Phase 2 places the
///     lock-free multiple-producer/single-consumer queue in front of this call.
class JsfxProcessor {
 public:
  /// Parse `source`, compile it, and run `@init` and the initial `@slider`.
  static Result<std::unique_ptr<JsfxProcessor>> create(
      std::string_view source, const JsfxEnvironment& environment = {},
      std::string source_name = {});

  /// Variant for callers that already parsed a document (e.g. an importer).
  static Result<std::unique_ptr<JsfxProcessor>> create(
      JsfxDocument document, const JsfxEnvironment& environment = {});

  ~JsfxProcessor();

  JsfxProcessor(const JsfxProcessor&) = delete;
  JsfxProcessor& operator=(const JsfxProcessor&) = delete;

  const JsfxDocument& document() const;

  /// Queue a 1-based slider value for the next `process` call. Returns false
  /// for an unknown slider index or a non-finite value.
  bool set_slider(int index, double value);
  double get_slider(int index) const;

  bool set_tempo(double bpm);
  bool set_play_state(int state);

  /// PRD Epic 5: call `@sample` once every `factor` frames and hold the last
  /// output (zero-order hold). Factors are clamped to [1, 8]. Apply from the
  /// processing thread, before `process`.
  static constexpr int kMaxDownsampleFactor = 8;
  void set_downsample_factor(int factor);
  int downsample_factor() const;

  /// PRD Epic 5: feed only `src[i].left` into `spl0`, run `@sample` once, and
  /// copy `spl0` to both output channels. Apply before `process`.
  void set_force_mono(bool enabled);
  bool force_mono() const;

  /// Process `frame_count` stereo frames. May be called in place
  /// (`input == output`). No allocation, locking or file I/O.
  void process(const AudioFrame* input, AudioFrame* output, int frame_count);

 private:
  struct Impl;
  explicit JsfxProcessor(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace jsfx
