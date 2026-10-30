#include "audio_stream_jsfx.h"

#include <algorithm>
#include <utility>

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>

using namespace godot;

namespace jsfx_godot {

// ---------------------------------------------------------------------------
// AudioStreamJSFX

void AudioStreamJSFX::_bind_methods() {
  ClassDB::bind_method(D_METHOD("set_jsfx", "jsfx"), &AudioStreamJSFX::set_jsfx);
  ClassDB::bind_method(D_METHOD("get_jsfx"), &AudioStreamJSFX::get_jsfx);
  ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "jsfx", PROPERTY_HINT_RESOURCE_TYPE,
                            "AudioEffectJSFX"),
               "set_jsfx", "get_jsfx");
}

void AudioStreamJSFX::set_jsfx(const Ref<AudioEffectJSFX>& jsfx) {
  if (jsfx == jsfx_) return;
  jsfx_ = jsfx;
  emit_changed();
}

Ref<AudioEffectJSFX> AudioStreamJSFX::get_jsfx() const { return jsfx_; }

Ref<AudioStreamPlayback> AudioStreamJSFX::_instantiate_playback() const {
  // Godot creates playbacks on the main thread (AudioStreamPlayer::play), where
  // compiling is allowed. Without a script the playback is silent.
  std::unique_ptr<jsfx::JsfxProcessor> processor;
  if (jsfx_.is_valid()) processor = jsfx_->create_processor();
  return Ref<AudioStreamPlayback>(
      memnew(AudioStreamPlaybackJSFX(jsfx_, std::move(processor))));
}

String AudioStreamJSFX::_get_stream_name() const {
  if (jsfx_.is_null()) return "JSFX";
  return jsfx_->get_script_path().get_file();
}

// ---------------------------------------------------------------------------
// AudioStreamPlaybackJSFX

AudioStreamPlaybackJSFX::AudioStreamPlaybackJSFX() : runtime_(nullptr, 1, false) {}

AudioStreamPlaybackJSFX::AudioStreamPlaybackJSFX(
    Ref<AudioEffectJSFX> jsfx, std::unique_ptr<jsfx::JsfxProcessor> processor)
    : runtime_(std::move(processor),
               jsfx.is_valid() ? jsfx->get_downsample_factor() : 1,
               jsfx.is_valid() && jsfx->get_force_mono()) {
  if (jsfx.is_valid()) {
    jsfx_id_ = ObjectID(jsfx->get_instance_id());
    jsfx->register_owner(this);
  }
}

AudioStreamPlaybackJSFX::~AudioStreamPlaybackJSFX() {
  // Godot releases playbacks in AudioServer::update() on the main thread, so
  // the EEL2 context is freed there, not on the audio thread.
  if (auto* jsfx = Object::cast_to<AudioEffectJSFX>(ObjectDB::get_instance(jsfx_id_))) {
    jsfx->unregister_owner(this);
  }
}

void AudioStreamPlaybackJSFX::_bind_methods() {
  ClassDB::bind_method(D_METHOD("reclaim_retired_processors"),
                       &AudioStreamPlaybackJSFX::reclaim_retired_processors);
}

void AudioStreamPlaybackJSFX::_start(double p_from_pos) {
  (void)p_from_pos;  // an instrument has no timeline to seek
  playing_.store(true, std::memory_order_release);
}

void AudioStreamPlaybackJSFX::_stop() {
  playing_.store(false, std::memory_order_release);
}

bool AudioStreamPlaybackJSFX::_is_playing() const {
  return playing_.load(std::memory_order_acquire);
}

double AudioStreamPlaybackJSFX::_get_playback_position() const {
  AudioServer* server = AudioServer::get_singleton();
  const double rate = server != nullptr ? server->get_mix_rate() : 0.0;
  return rate > 0.0 ? static_cast<double>(runtime_.processed_frames()) / rate : 0.0;
}

int32_t AudioStreamPlaybackJSFX::_mix(AudioFrame* p_buffer, double p_rate_scale,
                                      int32_t p_frames) {
  (void)p_rate_scale;  // the script runs at the mix rate; pitch_scale is unused
  if (p_buffer == nullptr || p_frames <= 0) return 0;
  // Instruments receive silence and write their output in place.
  std::fill(p_buffer, p_buffer + p_frames, AudioFrame{0.0f, 0.0f});
  if (!playing_.load(std::memory_order_acquire)) return 0;
  runtime_.process(p_buffer, p_buffer, p_frames);
  return p_frames;
}

void AudioStreamPlaybackJSFX::schedule_reclaim() {
  if (reclaim_scheduled_) return;
  reclaim_scheduled_ = connect_next_frame(this, "reclaim_retired_processors");
}

void AudioStreamPlaybackJSFX::reclaim_retired_processors() {
  reclaim_scheduled_ = false;
  if (runtime_.reclaim()) schedule_reclaim();
}

}  // namespace jsfx_godot
