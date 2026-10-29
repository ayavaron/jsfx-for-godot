#pragma once

#include <godot_cpp/godot.hpp>

namespace jsfx_godot {

void initialize_jsfx_module(godot::ModuleInitializationLevel level);
void uninitialize_jsfx_module(godot::ModuleInitializationLevel level);

}  // namespace jsfx_godot
