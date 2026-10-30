#pragma once

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>

#include "audio_effect_jsfx.h"
#include "jsfx_runtime.h"

namespace jsfx_godot {

/// A JSFX instrument as a sound source. Play it with AudioStreamPlayer, 2D or
/// 3D; each play() runs its own copy of the script with silent input, and MIDI
/// sent to `jsfx` (note_on() and friends) reaches every playing copy.
///
/// The script, its sliders, hot reload and imports live on the wrapped
/// AudioEffectJSFX, exactly as for bus effects.
class AudioStreamJSFX : public godot::AudioStream {
  GDCLASS(AudioStreamJSFX, godot::AudioStream)

 public:
  void set_jsfx(const godot::Ref<AudioEffectJSFX>& jsfx);
  godot::Ref<AudioEffectJSFX> get_jsfx() const;

  godot::Ref<godot::AudioStreamPlayback> _instantiate_playback() const override;
  godot::String _get_stream_name() const override;
  double _get_length() const override { return 0.0; }
  bool _is_monophonic() const override { return false; }

 protected:
  static void _bind_methods();

 private:
  godot::Ref<AudioEffectJSFX> jsfx_;
};

/// One playing copy of an AudioStreamJSFX. Runs until stopped.
class AudioStreamPlaybackJSFX final : public godot::AudioStreamPlayback,
                                      public JsfxRuntimeOwner {
  GDCLASS(AudioStreamPlaybackJSFX, godot::AudioStreamPlayback)

 public:
  AudioStreamPlaybackJSFX();
  AudioStreamPlaybackJSFX(godot::Ref<AudioEffectJSFX> jsfx,
                          std::unique_ptr<jsfx::JsfxProcessor> processor);
  ~AudioStreamPlaybackJSFX() override;

  void _start(double p_from_pos) override;
  void _stop() override;
  bool _is_playing() const override;
  int32_t _get_loop_count() const override { return 0; }
  double _get_playback_position() const override;
  void _seek(double /*p_position*/) override {}
  int32_t _mix(godot::AudioFrame* p_buffer, double p_rate_scale,
               int32_t p_frames) override;

  JsfxRuntime& jsfx_runtime() override { return runtime_; }
  void schedule_reclaim() override;

 protected:
  static void _bind_methods();

 private:
  void reclaim_retired_processors();

  /// Weak: Godot may keep a playback alive after shutdown (it never runs the
  /// AudioServer cleanup that releases it), and a strong reference would then
  /// keep the instrument's resource alive too.
  godot::ObjectID jsfx_id_;
  JsfxRuntime runtime_;
  std::atomic<bool> playing_{false};
  bool reclaim_scheduled_ = false;
};

}  // namespace jsfx_godot
