#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/audio_effect_instance.hpp>
#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/ref.hpp>

#include "audio_effect_jsfx.h"
#include "jsfx/processor.h"
#include "jsfx/slider_update_queue.h"

namespace jsfx_godot {

/// Per-bus instance. Owns the EEL2 processor and drains the parameter queue on
/// the audio thread. A null processor means passthrough (missing/invalid
/// script), which keeps the audio graph safe after a failed load.
class AudioEffectJSFXInstance final : public godot::AudioEffectInstance {
  GDCLASS(AudioEffectJSFXInstance, godot::AudioEffectInstance)

 public:
  AudioEffectJSFXInstance();
  AudioEffectJSFXInstance(godot::Ref<AudioEffectJSFX> effect,
                          std::unique_ptr<jsfx::JsfxProcessor> processor,
                          int downsample_factor, bool force_mono);
  ~AudioEffectJSFXInstance() override;

  void _process(const void* p_src_buffer, godot::AudioFrame* p_dst_buffer,
                int32_t p_frame_count) override;
  bool _process_silence() const override { return false; }

  bool push_slider(int index, double value);
  void set_downsample_factor(int factor);
  void set_force_mono(bool enabled);

  /// Diagnostic used by the Web/browser smoke test.
  std::uint64_t processed_frames() const {
    return processed_frames_.load(std::memory_order_relaxed);
  }

  /// Main-thread hot reload: publish a compiled replacement and retire the old
  /// processor. The old context is destroyed once the audio thread provably
  /// cannot hold it: it was idle at the swap, or has since left `_process`.
  void swap_processor(std::unique_ptr<jsfx::JsfxProcessor> processor);

 protected:
  static void _bind_methods();

 private:
  struct RetiredProcessor {
    jsfx::JsfxProcessor* processor = nullptr;
    /// `process_epoch_` observed right after the swap.
    std::uint64_t epoch_at_swap = 0;
  };

  bool can_reclaim(const RetiredProcessor& retired) const;

  void cleanup_retired_processors();
  void schedule_retired_cleanup();

  godot::Ref<AudioEffectJSFX> effect_;
  std::atomic<jsfx::JsfxProcessor*> processor_{nullptr};
  /// Incremented on entry to and exit from `_process`, so it is odd while the
  /// audio thread may be using a processor pointer it loaded.
  std::atomic<std::uint64_t> process_epoch_{0};
  std::atomic<std::uint64_t> processed_frames_{0};
  std::vector<RetiredProcessor> retired_;
  bool cleanup_scheduled_ = false;

  jsfx::SliderUpdateQueue queue_;
  std::atomic<int> downsample_factor_{1};
  std::atomic<bool> force_mono_{false};
};

}  // namespace jsfx_godot
