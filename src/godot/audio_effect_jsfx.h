#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_instance.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "jsfx/document.h"

namespace jsfx {
class JsfxProcessor;
}

namespace jsfx_godot {

class AudioEffectJSFXInstance;

/// Disconnects the per-frame EEL2 memory-pool refill and frees the pool.
void shutdown_ram_pool();

/// Godot resource wrapping one `.jsfx` file.
///
/// `script_path` is loaded and parsed on the main thread. Each bus instantiation
/// compiles its own EEL2 context (the processor owns it on the audio thread).
/// Slider values set through `set_slider` are broadcast to every live instance;
/// dynamic Godot properties and hot reload arrive in Phase 3.
class AudioEffectJSFX : public godot::AudioEffect {
  GDCLASS(AudioEffectJSFX, godot::AudioEffect)

 public:
  AudioEffectJSFX();
  ~AudioEffectJSFX() override;

  void set_script_path(const godot::String& path);
  godot::String get_script_path() const;

  void set_downsample_factor(int factor);
  int get_downsample_factor() const;

  void set_force_mono(bool enabled);
  bool get_force_mono() const;

  /// Queue a 1-based slider value for every live instance. Returns false for an
  /// unknown slider, a non-finite value, or a full parameter queue.
  bool set_slider(int index, double value);
  double get_slider(int index) const;

  /// Total audio frames processed by every live instance (diagnostics).
  int64_t get_processed_frames() const;

  /// Re-read and recompile `script_path` on the main thread, atomically swapping
  /// live instances to the new context. On failure the old script keeps playing.
  bool reload_script();

  /// Editor-only file watching; exported games can call reload_script().
  void set_auto_reload(bool enabled);
  bool get_auto_reload() const;

  /// Web builds: pre-zeroed EEL2 memory blocks (512 KiB each) kept ready for
  /// scripts that touch new memory while playing. See README "Memory".
  static void set_memory_pool_blocks(int blocks);
  static int get_memory_pool_blocks();

  godot::Ref<godot::AudioEffectInstance> _instantiate() override;

  /// Internal instance bookkeeping; called by AudioEffectJSFXInstance.
  void register_instance(AudioEffectJSFXInstance* instance);
  void unregister_instance(AudioEffectJSFXInstance* instance);

 protected:
  static void _bind_methods();

  // Godot's extension-object property hooks (additive to ClassDB properties).
  bool _set(const godot::StringName& p_name, const godot::Variant& p_value);
  bool _get(const godot::StringName& p_name, godot::Variant& r_value) const;
  void _get_property_list(godot::List<godot::PropertyInfo>* p_list) const;
  bool _property_can_revert(const godot::StringName& p_name) const;
  bool _property_get_revert(const godot::StringName& p_name,
                            godot::Variant& r_property) const;

 private:
  struct LoadedScript;

  LoadedScript build_script(const godot::String& path, bool preserve_values);
  void load_script_now(bool preserve_values, bool notify);
  void ensure_script_loaded() const;
  double effective_sample_rate() const;
  void ensure_file_watch();
  void _poll_script_file();
  const jsfx::JsfxSlider* find_slider_by_property(
      const godot::StringName& property) const;

  godot::String script_path_;
  jsfx::JsfxDocument document_;
  bool has_document_ = false;

  /// Set when a script_path was assigned off the main thread (for example by
  /// Godot's threaded ResourceFormatLoader). The first main-thread access parses
  /// and compiles, keeping EEL2 compilation off worker and audio threads.
  std::atomic<bool> pending_load_{false};

  /// Compiled when the script is set, so EEL2 syntax errors surface immediately
  /// and the first instantiation avoids a second compile. Only the main thread
  /// touches this; the instance that takes it owns it on the audio thread.
  std::unique_ptr<jsfx::JsfxProcessor> prepared_processor_;
  double prepared_sample_rate_ = 0.0;

  std::array<double, static_cast<std::size_t>(jsfx::JsfxDocument::kMaxSliders)>
      slider_values_{};
  std::array<bool, static_cast<std::size_t>(jsfx::JsfxDocument::kMaxSliders)>
      slider_set_{};

  /// Dynamic slider values written while a threaded resource load runs off the
  /// main thread. Merged into the document on the first main-thread load.
  std::array<double, static_cast<std::size_t>(jsfx::JsfxDocument::kMaxSliders)>
      pending_slider_values_{};
  std::array<bool, static_cast<std::size_t>(jsfx::JsfxDocument::kMaxSliders)>
      pending_slider_set_{};

  int downsample_factor_ = 1;
  bool force_mono_ = false;

  bool auto_reload_ = true;
  std::uint64_t script_modified_time_ = 0;
  int poll_tick_ = 0;
  bool file_watch_connected_ = false;

  mutable std::mutex instances_mutex_;
  std::vector<AudioEffectJSFXInstance*> instances_;
};

}  // namespace jsfx_godot
