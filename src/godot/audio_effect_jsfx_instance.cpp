#include "audio_effect_jsfx_instance.h"

#include <utility>

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace jsfx_godot {

AudioEffectJSFXInstance::AudioEffectJSFXInstance()
    : runtime_(nullptr, 1, false) {}

AudioEffectJSFXInstance::AudioEffectJSFXInstance(
    Ref<AudioEffectJSFX> effect, std::unique_ptr<jsfx::JsfxProcessor> processor,
    int downsample_factor, bool force_mono)
    : effect_(std::move(effect)),
      runtime_(std::move(processor), downsample_factor, force_mono) {
  if (effect_.is_valid()) effect_->register_owner(this);
}

AudioEffectJSFXInstance::~AudioEffectJSFXInstance() {
  if (effect_.is_valid()) effect_->unregister_owner(this);
}

void AudioEffectJSFXInstance::_bind_methods() {
  // Deferred reclamation target; internal class, not exposed to scripts.
  ClassDB::bind_method(D_METHOD("reclaim_retired_processors"),
                       &AudioEffectJSFXInstance::reclaim_retired_processors);
}

void AudioEffectJSFXInstance::_process(const void* p_src_buffer,
                                       AudioFrame* p_dst_buffer,
                                       int32_t p_frame_count) {
  runtime_.process(static_cast<const AudioFrame*>(p_src_buffer), p_dst_buffer,
                   p_frame_count);
}

void AudioEffectJSFXInstance::schedule_reclaim() {
  if (reclaim_scheduled_) return;
  reclaim_scheduled_ = connect_next_frame(this, "reclaim_retired_processors");
}

void AudioEffectJSFXInstance::reclaim_retired_processors() {
  reclaim_scheduled_ = false;
  if (runtime_.reclaim()) schedule_reclaim();
}

}  // namespace jsfx_godot
