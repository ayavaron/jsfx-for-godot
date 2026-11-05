#pragma once

#include <memory>

#include <godot_cpp/variant/string.hpp>

#include "jsfx/file_data.h"

namespace jsfx_godot {

/// Project setting naming the folder that stands in for REAPER's Data folder.
inline constexpr const char* kDataPathSetting = "audio/jsfx/data_path";

/// Registers `audio/jsfx/data_path` (default "res://") if the project lacks it.
void register_data_path_setting();

/// Files for `file_open` and file sliders, looked up like REAPER: next to the
/// script first, then in the data path, then at the project root. In exported
/// projects, audio files Godot imported (WAV, MP3) are read back from their
/// imported resources when the original file was not exported.
std::shared_ptr<jsfx::JsfxFileProvider> make_godot_file_provider(
    const godot::String& script_path);

}  // namespace jsfx_godot
