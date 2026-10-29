#include "audio_effect_jsfx_loader.h"

#include <godot_cpp/core/memory.hpp>

#include "audio_effect_jsfx.h"

using namespace godot;

namespace jsfx_godot {

PackedStringArray AudioEffectJSFXLoader::_get_recognized_extensions() const {
  PackedStringArray extensions;
  extensions.push_back("jsfx");
  return extensions;
}

bool AudioEffectJSFXLoader::_handles_type(const StringName& p_type) const {
  return p_type == StringName("AudioEffectJSFX") ||
         p_type == StringName("AudioEffect") ||
         p_type == StringName("Resource");
}

String AudioEffectJSFXLoader::_get_resource_type(const String& p_path) const {
  (void)p_path;
  return String("AudioEffectJSFX");
}

Variant AudioEffectJSFXLoader::_load(const String& p_path,
                                     const String& p_original_path,
                                     bool p_use_sub_threads,
                                     int32_t p_cache_mode) const {
  (void)p_use_sub_threads;
  (void)p_cache_mode;

  // Prefer the res:// path so serialized resources stay relocatable.
  const String script_path =
      p_original_path.is_empty() ? p_path : p_original_path;

  Ref<AudioEffectJSFX> effect(memnew(AudioEffectJSFX));
  effect->set_script_path(script_path);
  return effect;
}

}  // namespace jsfx_godot
