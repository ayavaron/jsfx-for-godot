#pragma once

#include <memory>

#include <godot_cpp/classes/audio_effect_instance.hpp>
#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/ref.hpp>

#include "audio_effect_jsfx.h"
#include "jsfx/processor.h"
#include "jsfx_runtime.h"

namespace jsfx_godot {

/// Per-bus-channel instance of an AudioEffectJSFX. The JSFX itself runs in
/// `runtime_`; this class adapts it to Godot's AudioEffectInstance.
class AudioEffectJSFXInstance final : public godot::AudioEffectInstance,
                                      public JsfxRuntimeOwner {
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

  JsfxRuntime& jsfx_runtime() override { return runtime_; }
  void schedule_reclaim() override;

 protected:
  static void _bind_methods();

 private:
  void reclaim_retired_processors();

  godot::Ref<AudioEffectJSFX> effect_;
  JsfxRuntime runtime_;
  bool reclaim_scheduled_ = false;
};

}  // namespace jsfx_godot
