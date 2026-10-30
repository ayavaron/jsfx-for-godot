#include "register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>

#include "audio_effect_jsfx.h"
#include "audio_effect_jsfx_instance.h"
#include "audio_effect_jsfx_loader.h"
#include "audio_stream_jsfx.h"

using namespace godot;

namespace {

Ref<jsfx_godot::AudioEffectJSFXLoader> jsfx_loader;

}  // namespace

namespace jsfx_godot {

void initialize_jsfx_module(ModuleInitializationLevel level) {
  if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

  GDREGISTER_CLASS(AudioEffectJSFX);
  // The engine calls AudioEffectInstance virtuals; it is not user-instantiable.
  GDREGISTER_INTERNAL_CLASS(AudioEffectJSFXInstance);
  GDREGISTER_INTERNAL_CLASS(AudioEffectJSFXLoader);
  GDREGISTER_CLASS(AudioStreamJSFX);
  // Created by AudioStreamJSFX::_instantiate_playback only.
  GDREGISTER_INTERNAL_CLASS(AudioStreamPlaybackJSFX);

  jsfx_loader = Ref<AudioEffectJSFXLoader>(memnew(AudioEffectJSFXLoader));
  ResourceLoader::get_singleton()->add_resource_format_loader(jsfx_loader);
}

void uninitialize_jsfx_module(ModuleInitializationLevel level) {
  if (level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

  shutdown_ram_pool();
  if (jsfx_loader.is_valid()) {
    ResourceLoader::get_singleton()->remove_resource_format_loader(jsfx_loader);
    jsfx_loader = Ref<AudioEffectJSFXLoader>();
  }
}

}  // namespace jsfx_godot

extern "C" {

GDExtensionBool GDE_EXPORT jsfx_for_godot_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization* r_initialization) {
  godot::GDExtensionBinding::InitObject init_obj(
      p_get_proc_address, p_library, r_initialization);

  init_obj.register_initializer(jsfx_godot::initialize_jsfx_module);
  init_obj.register_terminator(jsfx_godot::uninitialize_jsfx_module);
  init_obj.set_minimum_library_initialization_level(
      MODULE_INITIALIZATION_LEVEL_SCENE);

  return init_obj.init();
}

}  // extern "C"
