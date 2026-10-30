#include "jsfx/processor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

#include "jsfx/eel2_context.h"
#include "jsfx/ram_pool.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__) ||     (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define JSFX_HAS_MXCSR 1
#endif

namespace jsfx {
namespace {

/// Flush denormals to zero while JSFX code runs, as REAPER does. Decaying IIR
/// filters and reverbs otherwise produce subnormal values that cost 10-100x
/// per operation on x86. The caller's floating-point mode is restored after.
class ScopedFlushDenormals {
 public:
#ifdef JSFX_HAS_MXCSR
  ScopedFlushDenormals() : saved_(_mm_getcsr()) {
    _mm_setcsr(saved_ | 0x8040u);  // FTZ (bit 15) | DAZ (bit 6)
  }
  ~ScopedFlushDenormals() { _mm_setcsr(saved_); }

 private:
  unsigned int saved_;
#else
  ScopedFlushDenormals() {}
#endif
};

}  // namespace

struct JsfxProcessor::Impl {
  JsfxDocument document;
  std::unique_ptr<Eel2Context> context;

  static constexpr std::size_t kSliders = JsfxDocument::kMaxSliders;
  std::array<bool, kSliders> has_slider{};
  std::array<bool, kSliders> pending{};
  std::array<double, kSliders> pending_value{};
  std::array<double, kSliders> applied_value{};

  bool sliders_dirty = false;
  bool run_sample_loop = false;
  double* spl0 = nullptr;
  double* spl1 = nullptr;

  std::array<MidiMessage, kMaxMidiEventsPerBlock> midi_out{};
  int midi_out_count = 0;

  int downsample_factor = 1;
  bool force_mono = false;
  int downsample_phase = 0;
  double held_left = 0.0;
  double held_right = 0.0;
};

Result<std::unique_ptr<JsfxProcessor>> JsfxProcessor::create(
    std::string_view source, const JsfxEnvironment& environment,
    std::string source_name) {
  Result<JsfxDocument> parsed = JsfxDocument::parse(source, std::move(source_name));
  if (!parsed) {
    return Result<std::unique_ptr<JsfxProcessor>>::failure(parsed.error());
  }
  return create(std::move(parsed.value()), environment);
}

Result<std::unique_ptr<JsfxProcessor>> JsfxProcessor::create(
    JsfxDocument document, const JsfxEnvironment& environment) {
  Result<std::unique_ptr<Eel2Context>> context_result =
      Eel2Context::create(document, environment);
  if (!context_result) {
    return Result<std::unique_ptr<JsfxProcessor>>::failure(context_result.error());
  }

  auto impl = std::make_unique<Impl>();
  impl->document = std::move(document);
  impl->context = std::move(context_result.value());

  for (const JsfxSlider& slider : impl->document.sliders()) {
    const auto slot = static_cast<std::size_t>(slider.index - 1);
    impl->has_slider[slot] = true;
    impl->applied_value[slot] = impl->context->slider(slider.index);
    impl->pending_value[slot] = impl->applied_value[slot];
  }

  impl->spl0 = impl->context->variable("spl0");
  impl->spl1 = impl->context->variable("spl1");
  if (impl->spl0 == nullptr || impl->spl1 == nullptr) {
    return Result<std::unique_ptr<JsfxProcessor>>::failure(
        "failed to resolve JSFX output variables");
  }

  // A present but empty @sample is equivalent to passthrough; skip the loop.
  const JsfxSection& sample = impl->document.section(JsfxSectionKind::Sample);
  impl->run_sample_loop = sample.present && !sample.code.empty();

  return Result<std::unique_ptr<JsfxProcessor>>::success(
      std::unique_ptr<JsfxProcessor>(new JsfxProcessor(std::move(impl))));
}

JsfxProcessor::JsfxProcessor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

JsfxProcessor::~JsfxProcessor() = default;

const JsfxDocument& JsfxProcessor::document() const { return impl_->document; }

bool JsfxProcessor::set_slider(int index, double value) {
  if (impl_ == nullptr || index < 1 || index > JsfxDocument::kMaxSliders) return false;
  if (!std::isfinite(value)) return false;

  const auto slot = static_cast<std::size_t>(index - 1);
  if (!impl_->has_slider[slot]) return false;

  if (impl_->pending[slot]) {
    if (value == impl_->pending_value[slot]) return true;
  } else if (value == impl_->applied_value[slot]) {
    return true;
  }

  impl_->pending_value[slot] = value;
  impl_->pending[slot] = true;
  impl_->sliders_dirty = true;
  return true;
}

double JsfxProcessor::get_slider(int index) const {
  if (impl_ == nullptr || index < 1 || index > JsfxDocument::kMaxSliders) return 0.0;
  const auto slot = static_cast<std::size_t>(index - 1);
  if (!impl_->has_slider[slot]) return 0.0;
  return impl_->pending[slot] ? impl_->pending_value[slot]
                              : impl_->applied_value[slot];
}

bool JsfxProcessor::set_tempo(double bpm) {
  return impl_ != nullptr && impl_->context->set_tempo(bpm);
}

bool JsfxProcessor::set_play_state(int state) {
  return impl_ != nullptr && impl_->context->set_play_state(state);
}

void JsfxProcessor::set_downsample_factor(int factor) {
  if (impl_ == nullptr) return;
  factor = std::clamp(factor, 1, kMaxDownsampleFactor);
  if (factor == impl_->downsample_factor) return;
  impl_->downsample_factor = factor;
  impl_->downsample_phase = 0;
  impl_->held_left = 0.0;
  impl_->held_right = 0.0;
}

int JsfxProcessor::downsample_factor() const {
  return impl_ != nullptr ? impl_->downsample_factor : 1;
}

void JsfxProcessor::set_force_mono(bool enabled) {
  if (impl_ != nullptr) impl_->force_mono = enabled;
}

bool JsfxProcessor::force_mono() const {
  return impl_ != nullptr && impl_->force_mono;
}

bool JsfxProcessor::add_midi_input(const MidiMessage& message) {
  return impl_ != nullptr && impl_->context->push_midi_input(message);
}

std::span<const MidiMessage> JsfxProcessor::midi_output() const {
  if (impl_ == nullptr) return {};
  return {impl_->midi_out.data(), static_cast<std::size_t>(impl_->midi_out_count)};
}

void JsfxProcessor::process(const AudioFrame* input, AudioFrame* output,
                            int frame_count) {
  process_block(input, output, frame_count);
  if (impl_ == nullptr || frame_count <= 0) return;

  // Publish this block's midisend output, then drop unread MIDI input.
  impl_->midi_out_count =
      std::min(impl_->context->midi_output_count(), kMaxMidiEventsPerBlock);
  for (int i = 0; i < impl_->midi_out_count; ++i) {
    impl_->midi_out[static_cast<std::size_t>(i)] = impl_->context->midi_output(i);
  }
  impl_->context->clear_midi();
}

void JsfxProcessor::process_block(const AudioFrame* input, AudioFrame* output,
                                  int frame_count) {
  if (impl_ == nullptr || input == nullptr || output == nullptr ||
      frame_count <= 0) {
    return;
  }

  [[maybe_unused]] const ScopedFlushDenormals flush_denormals;
  // EEL2 memory first touched here comes from the pool, never from calloc.
  [[maybe_unused]] const ram_pool::ScopedRealtime realtime;

  if (impl_->sliders_dirty) {
    for (int index = 1; index <= JsfxDocument::kMaxSliders; ++index) {
      const auto slot = static_cast<std::size_t>(index - 1);
      if (!impl_->pending[slot]) continue;
      impl_->context->set_slider(index, impl_->pending_value[slot]);
      impl_->applied_value[slot] = impl_->pending_value[slot];
      impl_->pending[slot] = false;
    }
    impl_->context->run_slider();
    impl_->sliders_dirty = false;
  }

  impl_->context->run_block(frame_count);

  if (!impl_->run_sample_loop) {
    if (input != output) {
      std::copy(input, input + frame_count, output);
    }
    return;
  }

  double* spl0 = impl_->spl0;
  double* spl1 = impl_->spl1;
  if (spl0 == nullptr || spl1 == nullptr) return;

  const int factor = impl_->downsample_factor;
  const bool mono = impl_->force_mono;

  for (int i = 0; i < frame_count; ++i) {
    if (impl_->downsample_phase == 0) {
      *spl0 = static_cast<double>(input[i].left);
      if (!mono) *spl1 = static_cast<double>(input[i].right);
      impl_->context->run_sample();

      impl_->held_left = *spl0;
      impl_->held_right = mono ? *spl0 : *spl1;
      output[i].left = static_cast<float>(impl_->held_left);
      output[i].right = static_cast<float>(impl_->held_right);
    } else {
      // Zero-order hold for frames skipped by downsampling.
      output[i].left = static_cast<float>(impl_->held_left);
      output[i].right = static_cast<float>(impl_->held_right);
    }

    if (++impl_->downsample_phase >= factor) impl_->downsample_phase = 0;
  }
}

}  // namespace jsfx
