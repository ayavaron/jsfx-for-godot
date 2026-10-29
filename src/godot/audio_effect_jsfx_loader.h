#pragma once

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace jsfx_godot {

/// Makes `load("res://foo.jsfx")` return an AudioEffectJSFX resource.
/// Parse/compile failures are logged by AudioEffectJSFX::set_script_path and
/// leave a valid passthrough effect, so loading never crashes the engine.
class AudioEffectJSFXLoader : public godot::ResourceFormatLoader {
  GDCLASS(AudioEffectJSFXLoader, godot::ResourceFormatLoader)

 public:
  AudioEffectJSFXLoader() = default;
  ~AudioEffectJSFXLoader() override = default;

  godot::PackedStringArray _get_recognized_extensions() const override;
  bool _handles_type(const godot::StringName& p_type) const override;
  godot::String _get_resource_type(const godot::String& p_path) const override;
  godot::Variant _load(const godot::String& p_path,
                       const godot::String& p_original_path,
                       bool p_use_sub_threads,
                       int32_t p_cache_mode) const override;

 protected:
  static void _bind_methods() {}
};

}  // namespace jsfx_godot
